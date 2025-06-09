/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_DEVD_CACHE_MODE_H
#define NGNFS_DEVD_CACHE_MODE_H

int cache_mode_request(struct sockaddr_in *addr, u64 bnr, u8 mode, bool no_data);
int cache_mode_ack(struct sockaddr_in *addr, u64 bnr, u8 mode);
void cache_mode_process(u64 bnr);
void cache_release_all(struct sockaddr_in *addr);

#endif
