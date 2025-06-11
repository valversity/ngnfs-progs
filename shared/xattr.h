/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_XATTR_H
#define NGNFS_SHARED_XATTR_H

#include "shared/fs_info.h"
#include "shared/inode.h"

int ngnfs_xattr_get(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name,
			void *value, size_t val_size);
int ngnfs_xattr_remove(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name);
int ngnfs_xattr_set(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name,
		    void *value, size_t val_size, int flags);
int ngnfs_xattr_list(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, void *buf,
		     size_t size);

#endif
