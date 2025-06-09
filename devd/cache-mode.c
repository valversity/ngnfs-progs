/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/rbtree.h"

#include "shared/compare.h"
#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"

#include "utask/block.h"
#include "utask/net.h"
#include "utask/utask.h"

#include "devd/cache-mode.h"

/*
 * This governs the cached block modes that clients are allowed in their
 * caches across the network.
 *
 * A core challenge here is that the amount of memory we're willing to
 * spend tracking the mode of cached blocks limits the cache size of the
 * clients.  We want our tracking to be as dense as possible to make the
 * most of our memory resource and allow the largest caches in clients.
 *
 * This basic version just demonstrates the work flow, it totally
 * ignores memory consumption.  There's a lot of possibilities for
 * decreasing memory use and we need a baseline to measure from.
 *
 * The network protocol errs on the side of being overly chatty to
 * simplify the coordinated state transitions between this server and
 * the clients.  We can only have one sent mode in flight, and we won't
 * send another until we receive an ack.  This adds round trips to a few
 * exchanges, but means that the client can never receive confusing
 * messages that are sent concurrently can get re-ordered in flight.  We
 * could loosen this up but it'd need to be done very carefully and
 * makes either end need to deal with more possible transition
 * combinations.
 */

static struct cache_mode_instance {
	struct rb_root cli_root;

} global_cache_mode_inst = {
	.cli_root = RB_ROOT,
};

/*
 * @mode is the active mode that we must assume the client is currently
 * operating under.  There's assymetry as we set a more restrictive mode
 * immediately as it is sent to the client, but don't drop to a less
 * restrictive mode until we receive the ack from the client.
 *
 * @request is the next mode that will be set on the mode.  It is
 * primarily set by incoming requests from the client, but we can
 * request a mode of none to shrink caches.
 *
 * @sent is the mode that is in flight to the client.  We only allow one
 * mode in flight to simplify the protocol and allow us to aggressively
 * verify state transitions.
 *
 * @processing is set for the next request amongst the modes for a given
 * block that we're actively working towards.
 */
struct client_mode {
	struct rb_node node;
	struct sockaddr_in addr;
	u64 bnr;
	u8 mode;
	u8 request;
	u8 sent;
	u8 processing:1,
	   no_data:1;
};

/*
 * Returns true if the two modes can be granted to different clients.
 * It's always true as long as neither mode is a write.
 */
static inline bool compatible_modes(u8 a, u8 b)
{
	return a != NGNFS_CACHE_MODE_WRITE && b != NGNFS_CACHE_MODE_WRITE;
}

/*
 * Returns the least restrictive mode that's compatible with a requested
 * mode.  Only really serves to downgrade conflicting writes to reads
 * when a read is requested.
 */
static inline u8 most_compatible(u8 request)
{
	return request == NGNFS_CACHE_MODE_WRITE ? NGNFS_CACHE_MODE_NONE : NGNFS_CACHE_MODE_READ;
}

static inline struct client_mode *cli_container(struct rb_node *node)
{
	return node ? container_of(node, struct client_mode, node) : NULL;
}

static inline struct client_mode *next_cli(struct client_mode *cli)
{
	return cli ? cli_container(rb_next(&cli->node)) : NULL;
}

static inline struct client_mode *prev_cli(struct client_mode *cli)
{
	return cli ? cli_container(rb_prev(&cli->node)) : NULL;
}

static inline struct client_mode *next_cli_bnr(struct client_mode *cli)
{
	struct client_mode *tmp = next_cli(cli);

	return (tmp && tmp->bnr == cli->bnr) ? tmp : NULL;
}

static inline struct client_mode *prev_cli_bnr(struct client_mode *cli)
{
	struct client_mode *tmp = prev_cli(cli);

	return (tmp && tmp->bnr == cli->bnr) ? tmp : NULL;
}

static inline struct client_mode *first_cli_bnr(struct client_mode *cli)
{
	struct client_mode *tmp;

	while (cli && (tmp = prev_cli_bnr(cli)))
		cli = tmp;

	return cli;
}

/*
 * Iterate over every client mode for the starting from's bnr.  From is
 * only used to start iteration.  Only cli can be removed from the tree
 * in a given execution of the loop body.  cli is set to null if
 * iteration completes.
 */
#define for_each_cli_bnr(cli, tmp, from)		\
	for (cli = first_cli_bnr(from);			\
	     cli && ((tmp = next_cli_bnr(cli)), 1);	\
	     cli = tmp)

enum {
	GCM_ALLOC = (1 << 0),	/* allocate new cli if none found */
	GCM_NEXT = (1 << 1)	/* return next client within block */
};
static struct client_mode *get_client_mode(struct cache_mode_instance *inst, u64 bnr,
					   struct sockaddr_in *addr, int gcm)
{
	struct rb_node **node = &inst->cli_root.rb_node;
	struct rb_node *parent = NULL;
	struct client_mode *next = NULL;
	struct client_mode *cli = NULL;
	int cmp;

	while (*node) {
		parent = *node;
		cli = container_of(*node, struct client_mode, node);
		cmp = ngnfs_compare(bnr, cli->bnr) ?:
		      ngnfs_compare(ntohl(addr->sin_addr.s_addr),
				    ntohl(cli->addr.sin_addr.s_addr)) ?:
		      ngnfs_compare(ntohs(addr->sin_port), ntohs(cli->addr.sin_port));

		if (cmp < 0) {
			if (cli->bnr == bnr)
				next = cli;
			node = &(*node)->rb_left;
		} else if (cmp > 0) {
			node = &(*node)->rb_right;
		} else {
			return cli;
		}

		cli = NULL;
	}

	if (!cli && (gcm & GCM_NEXT))
		cli = next;

	if (!cli && (gcm & GCM_ALLOC)) {
		cli = calloc(1, sizeof(struct client_mode));
		if (cli) {
			cli->bnr = bnr;
			cli->addr = *addr;

			rb_link_node(&cli->node, parent, node);
			rb_insert_color(&cli->node, &inst->cli_root);
		}
	}

	return cli;
}

static void free_client_mode(struct cache_mode_instance *inst, struct client_mode *cli)
{
	rb_erase(&cli->node, &inst->cli_root);
	free(cli);
}

/*
 * Return the next client request to be processed.  If there isn't one
 * already marked then we select one at random from the conversion or
 * new request fifos in the array.
 */
static struct client_mode *find_processing_request(struct cache_mode_instance *inst, u64 bnr)
{
	struct client_mode *candidate = NULL;
	struct sockaddr_in addr = { };
	struct client_mode *start;
	struct client_mode *cli;
	struct client_mode *tmp;

	start = get_client_mode(inst, bnr, &addr, GCM_NEXT);
	if (!start)
		return NULL;

	for_each_cli_bnr(cli, tmp, start) {
		if (cli->processing)
			return cli;

		if (!candidate && cli->request && !cli->sent)
			candidate = cli;
	}

	if (candidate) {
		candidate->processing = 1;
		return candidate;
	}

	return NULL;
}

/*
 * The interface between components gets a little tricky here.  We've
 * extended block IO messages to include cache mode communication.  The
 * easy case to imagine is a client wanting to modify a block,
 * requesting the block contents and a write mode, and being granted
 * both.
 *
 * The more irritating flow is a client with a read mode requesting a
 * write.  It sends the write request.  But that can race with us
 * removing that client's read mode on behalf of a different client's
 * write mode.  After that's resolved, and we come back to sending the
 * write mode to the first client, it won't have the current block
 * contents anymore.  We can see that here and decide to send them the
 * current block contents along with the granted write mode.
 *
 * The end result is that utasks processing requests for a given block
 * can schedule to get block contents when sending a mode to a client.
 * It's a little surprising, but works out because the processing for a
 * given block is inherently tied up in its cached state.
 */
static int send_read_result(struct client_mode *cli, u64 bnr, u8 old_mode, u8 mode)
{
	struct ngnfs_msg_block_read_result rr;
	struct cached_block *cblk = NULL;
	struct page *data_page = NULL;
	struct ngnfs_msg_header hdr;
	int ret;
	int err;

	/* default to sending only the mode */
	rr.bnr = cpu_to_le64(bnr);
	rr.mode = mode;
	rr.err = 0;
	memset_zero_sizeof(rr._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(rr);
	hdr.type = NGNFS_MSG_BLOCK_READ_RESULT;
	data_page = NULL;

	if (old_mode == NGNFS_CACHE_MODE_NONE && mode != NGNFS_CACHE_MODE_NONE && !cli->no_data) {

		err = block_read(bnr, &cblk);
		if (err < 0) {
			/* send error on read io error */
			rr.mode = NGNFS_CACHE_MODE_NULL;
			rr.err = ngnfs_msg_err(err);
		} else {
			/* and include block data if they need it */
			hdr.data_size = cpu_to_le16(NGNFS_BLOCK_SIZE);
			data_page = block_data_page(cblk);
		}
	}

	ret = net_send(&cli->addr, &hdr, &rr, data_page);

	block_put(cblk);

	return ret;
}

/*
 * Send an unsolicited command to a client to set its mode to be
 * compatible with a conflicting mode that's being requested by another
 * client.
 */
static int send_mode_set(struct client_mode *cli, u64 bnr, u8 old_mode, u8 mode)
{
	struct ngnfs_msg_cache_mode cm;
	struct ngnfs_msg_header hdr;

	cm.bnr = cpu_to_le64(bnr);
	cm.mode = mode;
	memset_zero_sizeof(cm._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(cm);
	hdr.type = NGNFS_MSG_BLOCK_MODE_SET;

	return net_send(&cli->addr, &hdr, &cm, NULL);
}

/*
 * Advance the state machine across all the clients for a given block.
 * The driver of state changes are incoming requests.  If their
 * requested mode is incompatible with existing modes then we send
 * unsolicited mode changes.  As we get ack responses and update ther
 * client's mode they will be compatible with the request and we can
 * eventually send it a positive response.
 */
static int process_requests(struct cache_mode_instance *inst, u64 bnr)
{
	struct client_mode *cli;
	struct client_mode *req;
	struct client_mode *tmp;
	bool saw_incompat = false;
	u8 compat;
	int ret;

	while ((req = find_processing_request(inst, bnr))) {

		compat = most_compatible(req->request);
		saw_incompat = false;
		ret = 0;

		/* send compatible modes to any clients with incompatible modes */
		for_each_cli_bnr(cli, tmp, req) {
			if (cli == req)
				continue;
			if (compatible_modes(cli->mode, req->request))
				break;

			saw_incompat = true;

			if (!cli->sent) {
				ret = send_mode_set(cli, bnr, cli->mode, compat);
				if (ret < 0)
					goto out;

				cli->sent = compat;
			}
		}

		if (saw_incompat)
			break;

		/*
		 * This might schedule while reading block data.  We set
		 * ->sent first to prevent other utask callers from
		 * working on this request while we're reading.  Once we
		 * return the client tracking might have changed.  We
		 * only touch our request client before continuing at
		 * the end of the loop which fetches the next request
		 * and resets all state.
		 */
		req->sent = req->request;
		ret = send_read_result(req, bnr, req->mode, req->request);
		if (ret < 0) {
			req->sent = 0;
			goto out;
		}

		/* set more restrictive while sending, ack rx sets less restrictive */
		if (req->request > req->mode)
			req->mode = req->request;
		req->request = NGNFS_CACHE_MODE_NULL;
		req->processing = 0;
	}

	ret = 0;
out:
	return ret;
}

/*
 * Record an incoming cache mode request.  A read_result message will be
 * sent once the requested mode can be granted.  That might happen in
 * this call, but perhaps later after communicating with other clients.
 *
 * Must be called from a utask.
 */
int cache_mode_request(struct sockaddr_in *addr, u64 bnr, u8 mode, bool no_data)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_mode *cli;
	int ret;

	cli = get_client_mode(inst, bnr, addr, GCM_ALLOC);
	if (!cli) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * If we allocated the client couldn't have had a non-null mode.
	 * (XXX but we're trusting that this is happening, might be good
	 * to have the client send its current mode but then we have to
	 * deal with that racing with us adopting a more exclusive mode
	 * as we send.)
	 */
	if (cli->mode == NGNFS_CACHE_MODE_NULL)
		cli->mode = NGNFS_CACHE_MODE_NONE;

	/* client must only have one request in flight at a time */
	if (cli->request) {
		ret = -EPROTO;
		goto out;
	}

	cli->request = mode;
	cli->no_data = !!no_data;

	ret = process_requests(inst, bnr);
out:
	return ret;
}

/*
 * Received indication from a client that it has accepted the mode that
 * we sent it.  This can come in the form of explicit messages or
 * implicitly as clients write out dirty blocks.
 */
int cache_mode_ack(struct sockaddr_in *addr, u64 bnr, u8 mode)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_mode *cli;
	int ret;

	cli = get_client_mode(inst, bnr, addr, GCM_ALLOC);
	if (!cli) {
		ret = -ENOMEM;
		goto out;
	}

	/* we must only have one set msg in flight */
	if (cli->sent != mode) {
		ret = -EPROTO;
		goto out;
	}

	/* sending set more restrictive mode, we set less restrictive */
	if (cli->sent < cli->mode)
		cli->mode = cli->sent;
	cli->sent = NGNFS_CACHE_MODE_NULL;

	if (!cli->request && cli->mode == NGNFS_CACHE_MODE_NONE)
		free_client_mode(inst, cli);

	ret = process_requests(inst, bnr);
out:
	return ret;
}

/*
 * The client has been evicted from the cluster; remove all of its
 * cache modes on all blocks.
 */
void cache_release_all(struct sockaddr_in *addr)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;
	struct client_mode *cli;
	struct rb_node *walk;
	int cmp;

	walk = rb_first(&inst->cli_root);

	while (walk) {
		cli = cli_container(walk);
		walk = rb_next(walk); /* before we free it! */
		cmp = ngnfs_compare(ntohl(addr->sin_addr.s_addr),
				    ntohl(cli->addr.sin_addr.s_addr)) ?:
		      ngnfs_compare(ntohs(addr->sin_port), ntohs(cli->addr.sin_port));

		if (cmp == 0)
			free_client_mode(inst, cli);
	}
}

/*
 * Process requests for a given block without returning processing
 * errors to the caller.  There may be no recorded client modes for the
 * block (the caller can be processing an ack of the last client who was
 * sent a null mode).
 */
void cache_mode_process(u64 bnr)
{
	struct cache_mode_instance *inst = &global_cache_mode_inst;

	process_requests(inst, bnr);
}
