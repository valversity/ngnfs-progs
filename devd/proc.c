/* SPDX-License-Identifier: GPL-2.0 */

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"

#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"

#include "utask/net.h"
#include "utask/utask.h"

#include "devd/cache-mode.h"
#include "devd/bstore.h"
#include "devd/proc.h"

/*
 * The incoming control buf is allocated just after the proc_request
 * itself.
 */
struct proc_request {
	struct sockaddr_in addr;
	struct ngnfs_msg_header hdr;
	void *ctl_buf;
	struct page *data_page;
};

static void free_proc_request(struct proc_request *preq)
{
	if (preq) {
		if (preq->data_page)
			put_page(preq->data_page);
		free(preq);
	}
}

/*
 * The  main block_read handler just hands the request off to the cache
 * mode manager.  It will make sure that other clients have compatible
 * modes before sending the result.  It is responsible for sending the
 * block data along with the result, if necessary.
 */
static void block_read_utask(void *data)
{
	struct proc_request *preq = data;
	struct ngnfs_msg_block_read *br = preq->ctl_buf;
	const u64 bnr = le64_to_cpu(br->bnr);
	int ret;

	ret = cache_mode_request(&preq->addr, bnr, br->mode,
				 !!(le64_to_cpu(br->flags) & NGNFS_MSG_BLOCK_READ_FLAG_NO_DATA));
	BUG_ON(ret);

	free_proc_request(preq);
}

/*
 * If the sender is flushing in response to losing their write mode then
 * the write command is also a mode ack.  We only process the ack if the
 * IO succeeds.  The caller will resend writes as long as the block is
 * dirty.  (If they give up and drop the dirty block they can send a
 * mode ack).
 */
static void block_write_utask(void *data)
{
	struct proc_request *preq = data;
	struct ngnfs_msg_block_write *bw = preq->ctl_buf;
	struct ngnfs_msg_block_write_result wr;
	const u64 bnr = le64_to_cpu(bw->bnr);
	struct ngnfs_msg_header hdr;
	int ret;

	ret = bstore_write(bnr, preq->data_page);
	if (ret < 0)
		goto send;

	if (bw->mode)
		ret = cache_mode_ack(&preq->addr, bnr, bw->mode);
	else
		ret = 0;
send:
	wr.bnr = bw->bnr;
	wr.err = ngnfs_msg_err(ret);
	memset_zero_sizeof(wr._pad);

	hdr.data_size = 0;
	hdr.ctl_size = sizeof(wr);
	hdr.type = NGNFS_MSG_BLOCK_WRITE_RESULT;

	ret = net_send(&preq->addr, &hdr, &wr, NULL);
	BUG_ON(ret != 0); /* XXX reconnect? timeout? evict? */

	/* only process more cache mode requests after sending result including ack */
	cache_mode_process(bnr);

	free_proc_request(preq);
}

/*
 * Sent by the client when it can ack a mode request without having to
 * write the block, so everything but revoking write while the block is
 * dirty.
 */
static void block_mode_ack_utask(void *data)
{
	struct proc_request *preq = data;
	struct ngnfs_msg_cache_mode *cm = preq->ctl_buf;
	const u64 bnr = le64_to_cpu(cm->bnr);
	int ret;

	/* XXX verify */

	ret = cache_mode_ack(&preq->addr, bnr, cm->mode);
	BUG_ON(ret);

	free_proc_request(preq);
}

struct utask_fn_name {
	utask_fn_t fn;
	char *name;
};

static struct utask_fn_name proc_utask_fns[] = {
	[NGNFS_MSG_BLOCK_READ] = { block_read_utask, "bread" },
	[NGNFS_MSG_BLOCK_WRITE] = { block_write_utask, "bwrite" },
	[NGNFS_MSG_BLOCK_MODE_ACK] = { block_mode_ack_utask, "bmodeack" },
};

/*
 * This is called within a utask.
 */
int proc_recv(struct sockaddr_in *addr, struct ngnfs_msg_header *hdr, void *ctl_buf,
	      struct page *data_page)
{
	struct proc_request *preq;
	struct utask *tsk;
	utask_fn_t fn;
	char *name;
	int ret;

	if (hdr->type >= ARRAY_SIZE(proc_utask_fns) ||
	    ((fn = proc_utask_fns[hdr->type].fn) == NULL)) {
		ret = -EPROTO;
		goto out;
	}

	preq = malloc(sizeof(struct proc_request) + hdr->ctl_size);
	if (!preq) {
		ret = -ENOMEM;
		goto out;
	}

	preq->addr = *addr;
	preq->hdr = *hdr;
	preq->ctl_buf = (preq + 1);
	if (hdr->ctl_size)
		memcpy(preq->ctl_buf, ctl_buf, hdr->ctl_size);
	preq->data_page = data_page;
	if (data_page)
		get_page(data_page);

	name = proc_utask_fns[hdr->type].name;
	ret = utask_create_name(name, fn, preq, &tsk);
	if (ret < 0)
		free_proc_request(preq);
out:
	return ret;
}
