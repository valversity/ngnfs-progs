/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_MAP_H
#define NGNFS_SHARED_MAP_H

#include "shared/lk/in.h"
#include "shared/lk/list.h"
#include "shared/fs_info.h"

int ngnfs_map_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str);
int ngnfs_map_addrs_to_maps(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void ngnfs_map_free_addrs(struct list_head *addr_list);

struct ngnfs_msg_get_maps_result *ngnfs_maps_to_msg(struct ngnfs_fs_info *nfi);

int ngnfs_map_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr);

int ngnfs_maps_request(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr);

int ngnfs_map_setup(struct ngnfs_fs_info *nfi);
void ngnfs_map_destroy(struct ngnfs_fs_info *nfi);

int ngnfs_map_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *mapd_server_addr);
void ngnfs_map_client_destroy(struct ngnfs_fs_info *nfi);

#endif
