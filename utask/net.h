/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UTASK_NET_H__
#define __UTASK_NET_H__

#include "shared/lk/gfp.h"

#include "shared/format-msg.h"

struct sockaddr_in;

typedef int (*net_recv_fn_t)(struct sockaddr_in *addr, struct ngnfs_msg_header *hdr,
			     void *ctl_buf, struct page *data_page);
typedef void (*net_close_fn_t)(struct sockaddr_in *addr);

int net_send(struct sockaddr_in *addr, struct ngnfs_msg_header *hdr, void *ctl_buf,
	     struct page *data_page);

int net_listen(struct sockaddr_in *addr, net_close_fn_t close_fn);
int net_connect(struct sockaddr_in *addr);

int net_register_recv(net_recv_fn_t recv_fn);

int net_init(void);
void net_exit(void);

#endif
