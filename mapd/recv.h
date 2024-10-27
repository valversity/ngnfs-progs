/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_MAPD_RECV_H
#define NGNFS_MAPD_RECV_H

int mapd_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void mapd_destroy(struct ngnfs_fs_info *nfi);

#endif
