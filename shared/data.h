/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_DATA_H
#define NGNFS_SHARED_DATA_H

#include "shared/fs_info.h"
#include "shared/inode.h"

/*
 * The maximum amount of data we want to read/write in one transaction.
 */
#define NGNFS_DATA_MAX_IO (16 * NGNFS_BLOCK_SIZE)

ssize_t ngnfs_data_read(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			u64 offset, void *buf, size_t len);
ssize_t ngnfs_data_write(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			 u64 offset, void *buf, size_t len);

#endif
