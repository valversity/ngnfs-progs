/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_INODE_H
#define NGNFS_SHARED_INODE_H

#include "shared/block.h"
#include "shared/fs_info.h"
#include "shared/txn.h"

/*
 * Transactions work with references to inode struct in blocks.
 */
struct ngnfs_inode_txn_ref {
	struct ngnfs_txn_block *tblk;
	struct ngnfs_inode *ninode;
};

struct ngnfs_inode_ino_gen {
	u64 ino;
	u64 gen;
};

int ngnfs_inode_init(struct ngnfs_inode_txn_ref *itref, struct ngnfs_inode_ino_gen *ig, u32 nlink,
		     umode_t mode, u64 nsec, struct ngnfs_inode_ino_gen *parent_ig);
int ngnfs_inode_get(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, nbf_t nbf,
		    struct ngnfs_inode_ino_gen *ig, struct ngnfs_inode_txn_ref *itref);
int ngnfs_inode_alloc(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		      struct ngnfs_inode_ino_gen *ig, struct ngnfs_inode_txn_ref *itref);
int ngnfs_inode_read_copy(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			  void *buf, int size);
int ngnfs_inode_update(struct ngnfs_txn_block *tblk, struct ngnfs_inode *inode, s32 delta);

#endif
