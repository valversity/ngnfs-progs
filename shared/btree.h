/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BTREE_H
#define NGNFS_SHARED_BTREE_H

#include "shared/lk/limits.h"

#include "shared/format-block.h"
#include "shared/txn.h"

/*
 * The intent is for these iteration functions to be able to return
 * -errno, 0, .. INT_MAX to be able to stop iteration and return those
 * values to the caller.  If _ITER_CONTINUE is returned then the return
 * is ignored and iteration continues.  It's a negated impossibly large
 * errno so it shouldn't be confused with valid -errno.
 */
#define NGNFS_BTREE_ITER_CONTINUE	INT_MIN

typedef int (*ngnfs_btree_read_iter_fn_t)(struct ngnfs_btree_key *key, void *val,
					  size_t val_size, void *arg);

/*
 * btree operations. 0 should be always be a no-op to catch bugs.
 *
 * BOP_PREPARE is used to pre-split or merge parent (non-leaf) nodes
 * while traversing to return a writeable leaf so that the caller can do
 * at least one operation without re-traversing the btree. We don't know
 * whether the caller will insert, delete, or replace, or how big the
 * value will be. So we pre-split any parent that can't fit another
 * block ref, and pre-merge any parent if removing a block ref would
 * bring it below the merge threshold.
 */
enum {
	BOP_NOOP = 0,
	BOP_INSERT,
	BOP_DELETE,
	BOP_REPLACE,
	BOP_PREPARE,
};

/*
 * Describes the btree operation that should be done.
 */
struct ngnfs_btree_op {
	struct ngnfs_btree_key key;
	void *val;
	size_t val_size;
	size_t old_size;	/* 0 or size of value to replace */
	unsigned op;
};

/*
 * This is called for every existing item within the caller's key/last
 * range.  It is always called on the last key, and if an item doesn't
 * exist at that key then val will be NULL and size will be 0.
 *
 * If a -errno is returned then iteration stops, the op argument is
 * ignored, and that error is returned to the caller.
 *
 * When a -errno isn't returned then the op struct is checked to see if
 * an operation should be performed at the key.  It can be left
 * initialized to 0 to not perform any operation.
 *
 * After performing the operation, if the return wasn't
 * NGNFS_BTREE_ITER_CONTINUE then iteration stops and the non-_CONTINUE
 * status is returned to the caller.
 *
 * If an attempt to perform an operation fails then it can return an
 * error that overrides whatever may have been returned by the iter fn.
 */
typedef int (*ngnfs_btree_write_iter_fn_t)(struct ngnfs_btree_key *key, void *val, size_t size,
					   void *arg, struct ngnfs_btree_op *op);

void ngnfs_btree_key_inc(struct ngnfs_btree_key *key);
void ngnfs_btree_key_set_min(struct ngnfs_btree_key *key);
bool ngnfs_btree_key_is_min(struct ngnfs_btree_key *key);
void ngnfs_btree_key_set_max(struct ngnfs_btree_key *key);
bool ngnfs_btree_key_is_max(struct ngnfs_btree_key *key);

int ngnfs_btree_read_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			  struct ngnfs_btree_root *root, struct ngnfs_btree_key *key,
			  struct ngnfs_btree_key *next, struct ngnfs_btree_key *last,
			  ngnfs_btree_read_iter_fn_t iter, void *iter_arg);
int ngnfs_btree_write_iter(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			   struct ngnfs_txn_block *root_tblk, struct ngnfs_btree_root *root,
			   struct ngnfs_btree_key *key, struct ngnfs_btree_key *last,
			   ngnfs_btree_write_iter_fn_t iter, void *iter_arg);

#endif
