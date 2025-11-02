/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/rbtree.h"
#include "shared/lk/uio.h"

#include "shared/compare.h"
#include "shared/format-block.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"
#include "shared/valgrind_support.h"

#include "utask/net.h"
#include "utask/utask.h"

/*
 * This is the most basic attempt at driving socket networking in our
 * utask runtime using io_uring.
 *
 * To keep it simple, we have utasks prepare and synchronously wait for
 * completion of individual io_uring submissions.  We'll want to evolve
 * this quite a bit to avoid kernel entries and buffer copies.
 *
 * This blurs the lines between instances of (the only supported tcp)
 * transport sockets and peer communication state.  We should support
 * retransmitting unacknowledged sends down a reconnected transport to a
 * previously existing peer.  This will make more sense as maps come
 * online which will govern peer lifetimes and map transport addresses
 * to peer identifiers.
 */

static struct net_instance {
	struct rb_root peer_root;
	net_recv_fn_t recv_fn;
	struct utask *destroy_tsk;
	struct list_head sockets_list;
	struct list_head destroy_list;

} global_net_inst = {
	.peer_root = RB_ROOT,
	.sockets_list = LIST_HEAD_INIT(global_net_inst.sockets_list),
	.destroy_list = LIST_HEAD_INIT(global_net_inst.destroy_list),
};

struct net_socket {
	struct rb_node node;
	struct sockaddr_in addr;
	struct list_head send_queue;
	struct list_head inst_head;
	int fd;

	struct utask *accept_tsk;
	struct utask *connect_tsk;
	struct utask *send_tsk;
	struct utask *recv_tsk;

	net_close_fn_t close_fn;
};

/*
 * The control buf is allocated.  The data page is a reference that is
 * dropped once we've copied it into the socket.
 */
struct net_send_entry {
	struct list_head head;
	struct ngnfs_msg_header hdr;
	void *ctl_buf;
	struct page *data_page;
};

static struct net_socket *alloc_sock(struct net_instance *inst)
{
	struct net_socket *sock;

	sock = calloc(1, sizeof(struct net_socket));
	if (sock) {
		RB_CLEAR_NODE(&sock->node);
		INIT_LIST_HEAD(&sock->send_queue);
		sock->fd = -1;

		list_add_tail(&sock->inst_head, &inst->sockets_list);
	}

	return sock;
}

static void cancel_fd_waiter(int fd)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
	utask_wait_event_nocancel(waiter.completed);

	/* it's OK to not find any pending submissions */
	BUG_ON(waiter.res < 0 && waiter.res != -ENOENT);
}

static void free_ent(struct net_send_entry *ent)
{
	if (!list_empty(&ent->head))
		list_del_init(&ent->head);
	if (ent->data_page)
		put_page(ent->data_page);
	free(ent);
}

/*
 * Free all the resources associated with the sock.  The caller can't
 * touch the sock after this is called.
 *
 * The calling utask itself won't be destroyed if it's one of the sock
 * tasks destroyed by _other.  The caller will be destroyed when it
 * returns.
 *
 * io_uring commands might have references to these task stacks via
 * waiters.  So we stop activity on the socket and cancel all ops before
 * tearing down the tasks.
 */
static void free_sock(struct net_instance *inst, struct net_socket *sock)
{
	struct net_send_entry *ent;
	struct net_send_entry *tmp;

	/* I think shutdown is non-blocking?   hmm. */
	if (sock->fd >= 0) {
		shutdown(sock->fd, SHUT_RDWR);
		cancel_fd_waiter(sock->fd);
	}

	utask_destroy(sock->accept_tsk);
	utask_destroy(sock->send_tsk);
	utask_destroy(sock->recv_tsk);

	if (sock->fd >= 0)
		close(sock->fd);

	list_for_each_entry_safe(ent, tmp, &sock->send_queue, head)
		free_ent(ent);

	if (sock->close_fn)
		sock->close_fn(&sock->addr);

	if (!RB_EMPTY_NODE(&sock->node))
		rb_erase(&sock->node, &inst->peer_root);
	if (!list_empty(&sock->inst_head))
		list_del_init(&sock->inst_head);
	free(sock);
}

/*
 * Freeing sockets can wait for other utasks or canceled io_uring ops.
 * The exiting won't cancel us until we're done with the destroy list.
 */
static void destroy_utask(void *data)
{
	struct net_instance *inst = data;
	struct net_socket *sock;
	struct net_socket *tmp;

	do {
		utask_wait_event_task(!list_empty(&inst->destroy_list));

		list_for_each_entry_safe(sock, tmp, &inst->destroy_list, inst_head)
			free_sock(inst, sock);

	} while (!utask_am_canceled());
}

static void destroy_sock(struct net_instance *inst, struct net_socket *sock)
{
	list_move_tail(&sock->inst_head, &inst->destroy_list);
	utask_wake_task(inst->destroy_tsk);
}

static struct net_socket *get_peer_sock(struct net_instance *inst, struct sockaddr_in *addr,
					struct net_socket *insert)
{
	struct rb_node **node = &inst->peer_root.rb_node;
	struct rb_node *parent = NULL;
	struct net_socket *sock;
	int cmp;

	while (*node) {
		parent = *node;
		sock = container_of(*node, struct net_socket, node);
		cmp = ngnfs_compare(ntohl(addr->sin_addr.s_addr),
				    ntohl(sock->addr.sin_addr.s_addr)) ?:
		      ngnfs_compare(ntohs(addr->sin_port), ntohs(sock->addr.sin_port));

		if (cmp < 0)
			node = &(*node)->rb_left;
		else if (cmp > 0)
			node = &(*node)->rb_right;
		else
			break;

		sock = NULL;
	}

	if (!sock && insert) {
		sock = insert;
		rb_link_node(&sock->node, parent, node);
		rb_insert_color(&sock->node, &inst->peer_root);
	}

	return sock;
}

/*
 * The uring net paths use MSG_WAITALL for both send and recv to
 * transfer the entire buffer/iovec.  The _waiter callers rely on the
 * _waiter functions to ensure this happens.  I'm not sure how rigorous
 * it is, can it be truncated by signals?
 */
static ssize_t sendmsg_waiter(int fd, struct msghdr *msg, int flags)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	size_t full_len;
	int ret;

	full_len = iov_length(msg->msg_iov, msg->msg_iovlen);

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_sendmsg(sqe, fd, msg, flags | MSG_WAITALL);
	ret = utask_wait_event_task(waiter.completed);

	BUG_ON(ret == 0 && waiter.res >= 0 && waiter.res != full_len);

	if (ret == 0)
		ret = waiter.res;

	return ret;
}

static ssize_t recvmsg_waiter(int fd, struct msghdr *msg, int flags)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	size_t full_len;
	int ret;

	full_len = iov_length(msg->msg_iov, msg->msg_iovlen);

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_recvmsg(sqe, fd, msg, flags | MSG_WAITALL);
	ret = utask_wait_event_task(waiter.completed);

	BUG_ON(ret == 0 && waiter.res >= 0 && waiter.res != full_len);
	if (ret == 0)
		ret = waiter.res;

	return ret;
}

static ssize_t recv_waiter(int fd, void *buf, size_t size, int flags)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_recv(sqe, fd, buf, size, flags | MSG_WAITALL);
	ret = utask_wait_event_task(waiter.completed);

	/* == 0 when fd is shutdown/disconnected */
	BUG_ON(ret == 0 && waiter.res > 0 && waiter.res != size);

	if (ret == 0)
	       ret = waiter.res;

	return ret;
}

static void send_utask(void *data)
{
	struct net_instance *inst = &global_net_inst;
	struct net_socket *sock = data;
	struct net_send_entry *ent;
	struct iovec iov[3];
	struct msghdr msg;
	int ret;
	int nr;

	for (;;) {
		ret = utask_wait_event_task(!list_empty(&sock->send_queue));
		if (ret < 0)
			break;

		while ((ent = list_first_entry_or_null(&sock->send_queue,
						       struct net_send_entry, head))) {

			nr = 0;
			iov[nr].iov_base = &ent->hdr;
			iov[nr++].iov_len = sizeof(struct ngnfs_msg_header);
			if (ent->hdr.ctl_size) {
				iov[nr].iov_base = ent->ctl_buf;
				iov[nr++].iov_len = ent->hdr.ctl_size;
			}
			if (ent->hdr.data_size) {
				iov[nr].iov_base = page_address(ent->data_page);
				iov[nr++].iov_len = le16_to_cpu(ent->hdr.data_size);
			}

			memset_zero_sizeof(msg);
			msg.msg_iov = iov;
			msg.msg_iovlen = nr;

			ret = sendmsg_waiter(sock->fd, &msg, 0);
			if (ret < 0)
				break;

			free_ent(ent);
		}
	}

	destroy_sock(inst, sock);
}

static void recv_utask(void *data)
{
	struct net_instance *inst = &global_net_inst;
	struct net_socket *sock = data;
	struct page *data_page = NULL;
	struct ngnfs_msg_header hdr;
	void *ctl_buf = NULL;
	struct iovec iov[2];
	struct msghdr msg;
	int ret;
	int nr;

	ctl_buf = malloc(NGNFS_BLOCK_SIZE);
	if (!ctl_buf) {
		ret = -ENOMEM;
		goto out;
	}

	VGS_INIT_BUF(&hdr, sizeof(hdr));
	VGS_INIT_BUF(ctl_buf, NGNFS_BLOCK_SIZE);

	for (;;) {
		ret = recv_waiter(sock->fd, &hdr, sizeof(struct ngnfs_msg_header), 0);
		if (ret <= 0)
			goto out;

		/* check header */
		ret = ngnfs_msg_verify_header(&hdr);
		if (ret < 0)
			goto out;

		nr = 0;
		if (hdr.ctl_size) {
			iov[nr].iov_base = ctl_buf;
			iov[nr++].iov_len = hdr.ctl_size;
		}
		if (hdr.data_size) {
			if (data_page == NULL) {
				data_page = alloc_page(GFP_NOFS);
				if (!data_page) {
					ret = -ENOMEM;
					goto out;
				}
				VGS_INIT_BUF(page_address(data_page), PAGE_SIZE);
			}

			iov[nr].iov_base = page_address(data_page);
			iov[nr++].iov_len = le16_to_cpu(hdr.data_size);
		}

		if (nr > 0) {
			memset_zero_sizeof(msg);
			msg.msg_iov = iov;
			msg.msg_iovlen = nr;

			ret = recvmsg_waiter(sock->fd, &msg, 0);
			if (ret < 0)
				goto out;
		}

		ret = inst->recv_fn(&sock->addr, &hdr, ctl_buf, data_page);
		if (ret < 0)
			goto out;

		/* get a new page 'cause the rx fn is using ours */
		if (data_page && page_ref_count(data_page) > 1) {
			put_page(data_page);
			data_page = NULL;
		}
	}

out:
	free(ctl_buf);
	if (data_page)
		put_page(data_page);

	destroy_sock(inst, sock);
}

/*
 * This interface only allows sending to connections that were
 * previously created with _connect or _accept.
 */
int net_send(struct sockaddr_in *addr, struct ngnfs_msg_header *hdr, void *ctl_buf,
	     struct page *data_page)
{
	struct net_instance *inst = &global_net_inst;
	struct net_send_entry *ent;
	struct net_socket *sock;
	int ret;

	sock = get_peer_sock(inst, addr, NULL);
	if (!sock) {
		ret = -EADDRNOTAVAIL;
		goto out;
	}

	ent = malloc(sizeof(struct net_send_entry) + hdr->ctl_size);
	if (!ent) {
		ret = -ENOMEM;
		goto out;
	}

	ent->ctl_buf = (ent + 1);
	ent->hdr = *hdr;
	memcpy(ent->ctl_buf, ctl_buf, hdr->ctl_size);
	if (data_page) {
		ent->data_page = data_page;
		get_page(data_page);
	} else {
		ent->data_page = NULL;
	}

	list_add_tail(&ent->head, &sock->send_queue);
	utask_wake_task(sock->send_tsk);
	ret = 0;
out:
	return ret;
}

static int accept_waiter(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
	ret = utask_wait_event_task(waiter.completed);
	if (ret == 0)
		ret = waiter.res;

	return ret;
}

static void accept_utask(void *data)
{
	struct net_instance *inst = &global_net_inst;
	struct net_socket *sock = data;
	struct net_socket *accepted;
	struct net_socket *found;
	struct sockaddr_in addr;
	socklen_t socklen;
	int ret;
	int fd;

	VGS_INIT_BUF(&addr, sizeof(addr));

	for (;;) {
		socklen = sizeof(struct sockaddr_in);
		ret = accept_waiter(sock->fd, (struct sockaddr *)&addr, &socklen, 0);
		if (ret >= 0 && (socklen != sizeof(addr) || sock->addr.sin_family != AF_INET))
			ret = -EAFNOSUPPORT;
		if (ret < 0)
			goto out;
		fd = ret;

		accepted = alloc_sock(inst);
		if (!accepted) {
			ret = -ENOMEM;
			close(fd);
			goto out;
		}

		found = get_peer_sock(inst, &addr, accepted);
		BUG_ON(found != accepted); /* XXX don't yet support racing reconnect */

		accepted->addr = addr;
		accepted->fd = fd;
		accepted->close_fn = sock->close_fn;

		ret = utask_create(send_utask, accepted, &accepted->send_tsk) ?:
		      utask_create(recv_utask, accepted, &accepted->recv_tsk);
		if (ret < 0) {
			destroy_sock(inst, accepted);
			continue;
		}
	}

out:
	destroy_sock(inst, sock);
}

/*
 * An initialization routine that starts the accept utask which spins
 * accepting new connected sockets on the listening socket.  This
 * doesn't need to be called from a utask.
 */
int net_listen(struct sockaddr_in *addr, net_close_fn_t close_fn)
{
	struct net_instance *inst = &global_net_inst;
	struct net_socket *sock = NULL;
	int fd = -1;
	int optval;
	int ret;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	optval = 1;
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	ret = bind(fd, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	ret = listen(fd, 255);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	sock = alloc_sock(inst);
	if (!sock) {
		ret = -ENOMEM;
		goto out;
	}

	sock->close_fn = close_fn;
	ret = utask_create(accept_utask, sock, &sock->accept_tsk);
	if (ret < 0) {
		destroy_sock(inst, sock);
		goto out;
	}

	sock->addr = *addr;
	sock->fd = fd;
	fd = -1;

	ret = 0;
out:
	if (ret < 0 && fd >= 0)
		close(fd);

	return ret;
}

static int connect_waiter(int fd, struct sockaddr *addr, socklen_t addrlen)
{
	struct utask_cqe_waiter waiter;
	struct io_uring_sqe *sqe;
	int ret;

	sqe = utask_get_sqe_waiter(&waiter);
	io_uring_prep_connect(sqe, fd, addr, addrlen);
	ret = utask_wait_event_task(waiter.completed);
	if (ret == 0)
		ret = waiter.res;

	return ret;
}

/*
 * Unlike _listen, this must be called from a utask.  It will schedule
 * the caller while waiting for the connect to complete.
 */
int net_connect(struct sockaddr_in *addr)
{
	struct net_instance *inst = &global_net_inst;
	struct net_socket *sock;
	int fd = -1;
	int ret;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	ret = connect_waiter(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
	if (ret < 0)
		goto out;

	sock = alloc_sock(inst);
	if (!sock) {
		ret = -ENOMEM;
		goto out;
	}

	sock->addr = *addr;
	sock->fd = fd;
	fd = -1;

	ret = utask_create(send_utask, sock, &sock->send_tsk) ?:
	      utask_create(recv_utask, sock, &sock->recv_tsk);
	if (ret < 0)
		destroy_sock(inst, sock);

out:
	if (ret < 0 && fd >= 0)
		close(fd);

	return ret;
}

/*
 * Connect to mapd and tell it we are a devd listening at listen_addr.
 */
int net_connect_mapd(struct sockaddr_in *listen_addr, struct sockaddr_in *mapd_addr)
{
	struct ngnfs_msg_devd_hello hello;
	struct ngnfs_ipv4_addr *addr = &hello.addr;
	struct ngnfs_msg_header hdr;
	int ret;

	/* set up the message */
	addr->device_uuid = 0; /* XXX also need a connection uuid*/
	addr->addr = cpu_to_le32(listen_addr->sin_addr.s_addr);
	addr->port = cpu_to_le16(listen_addr->sin_port);

	hdr.crc = 0; /* not necessary for non-data messages */
	hdr.ctl_size = sizeof(hello);
	hdr.data_size = 0;
	hdr.type = NGNFS_MSG_DEVD_HELLO;

	ret = net_connect(mapd_addr);
	if (ret < 0)
		goto out;

	/* send a message */
	ret = net_send(mapd_addr, &hdr, &hello, NULL);
	if (ret < 0)
		goto out;

	/* receive the ack */

	/* XXX do something if it dies */
out:
	return ret;
}

/*
 * All receives coming through this processes networking layer are
 * handled through one incoming recv callback.
 */
int net_register_recv(net_recv_fn_t recv_fn)
{
	struct net_instance *inst = &global_net_inst;

	inst->recv_fn = recv_fn;

	return 0;
}

int net_init(void)
{
	struct net_instance *inst = &global_net_inst;

	return utask_create_nowake(destroy_utask, inst, &inst->destroy_tsk);
}

void net_exit(void)
{
	struct net_instance *inst = &global_net_inst;

	inst->recv_fn = NULL;

	list_splice_init(&inst->sockets_list, &inst->destroy_list);
	if (inst->destroy_tsk) {
		utask_wake_task(inst->destroy_tsk);
		utask_destroy(inst->destroy_tsk);
		inst->destroy_tsk = NULL;
	}
}
