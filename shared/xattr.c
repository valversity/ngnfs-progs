/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/types.h"
#include "shared/lk/xattr.h"
#include "shared/lk/xxhash.h"

#include "shared/block.h"
#include "shared/btree.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"
#include "shared/xattr.h"

/*
 * The btree key for each xattr is:
 *
 *   { <hash of name>, <unique counter value>, 0 }
 *
 * Once we have iterated over all the keys with a matching hash but no
 * matching name, as signified by calling the callback with a null val
 * pointer, we can assume that nothing with that name exists.
 */
static u64 xattr_hash(void *name, size_t name_len)
{
	return xxh64(name, name_len, NGNFS_XATTR_HASH_SEED);
}

static bool xattr_names_equal(u8 *a, size_t a_len, u8 *b, size_t b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/*
 * Arguments for passing down to the xattr btree iter callback function
 * and back up to the caller.
 */
struct xattr_args {
	struct ngnfs_inode_txn_ref *ino;
	u64 hash;

	struct ngnfs_xattr *xattr;
	int xattr_size;

	char *name;
	u8 name_len;
	char *value;
	u16 val_size;

	int flags;

	s32 names_delta;	/* change to size of all names for listxattr */
	bool found;
};

static size_t xattr_size(size_t name_len, size_t val_size)
{
	return offsetof(struct ngnfs_xattr, name) + name_len + val_size;
}

static void init_xattr_args(struct xattr_args *xa, struct ngnfs_inode_txn_ref *ino,
			    char *name, size_t name_len, void *value, size_t val_size,
			    struct ngnfs_xattr *xattr, int flags)
{
	xa->ino = ino;
	xa->hash = xattr_hash(name, name_len);
	xa->xattr = xattr;
	xa->name = name;
	xa->name_len = name_len;
	xa->value = value;
	xa->val_size = val_size;
	xa->flags = flags;

	if (xattr) {
		xa->xattr_size = xattr_size(name_len, val_size);
		xattr->name_len = name_len;
		xattr->val_len = cpu_to_le16(val_size);
		memcpy(xattr->name, name, name_len);
		memcpy(xattr->name + name_len, value, val_size);
	} else {
		xa->xattr_size = 0;
	}
}

/*
 * Reset the members that may be altered during a transaction and need
 * to be reset before retrying a transaction.
 */
static void reset_xattr_args(struct xattr_args *xa)
{
	xa->names_delta = 0;
	xa->found = false;
}

static void init_xattr_key(struct ngnfs_btree_key *key, u64 hash, u64 counter)
{
	*key = (struct ngnfs_btree_key) {
		.k[0] = cpu_to_le64(hash),
		.k[1] = cpu_to_le64(counter),
	};
}

/*
 * When setting an xattr, set the secondary btree key to the unique
 * xattr create counter value in the inode, and increment the inode
 * counter.
 */
static void update_xattr_key(struct ngnfs_btree_key *key, u64 hash, struct ngnfs_inode_txn_ref *ino)
{
	*key = (struct ngnfs_btree_key) {
		.k[0] = cpu_to_le64(hash),
		.k[1] = ino->ninode->xattr_creates,
	};

	ngnfs_tblk_assign(ino->tblk, ino->ninode->xattr_creates,
			  cpu_to_le64(le64_to_cpu(key->k[1]) + 1));
}

static int fill_xattr_rd(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	if (!xattr_names_equal(xattr->name, xattr->name_len, (u8 *) xa->name, xa->name_len))
		return NGNFS_BTREE_ITER_CONTINUE;

	if (le16_to_cpu(xattr->val_len) > xa->val_size)
		return -ENOBUFS;

	xa->found = true;
	xa->val_size = le16_to_cpu(xattr->val_len);
	memcpy(xa->value, xattr->name + xattr->name_len, xa->val_size);

	return 0;
}

static int get_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		     struct ngnfs_inode_txn_ref *ino, struct xattr_args *xa)
{
	struct ngnfs_btree_key key;
	int ret;

	init_xattr_key(&key, xa->hash, 0);

	ret = ngnfs_btree_read_iter(nfi, txn, &ino->ninode->xattrs, &key, NULL, NULL,
				    fill_xattr_rd, xa);

	if (ret < 0)
		goto out;

	if (!xa->found)
		ret = -ENODATA;
out:
	return ret;
}

int ngnfs_xattr_get(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name,
		    void *value, size_t val_size)
{
	struct ngnfs_transaction txn;
	struct ngnfs_inode_txn_ref ino;
	struct xattr_args xa;
	size_t name_len;
	int ret;

	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;

	ngnfs_txn_init(&txn);
	init_xattr_args(&xa, &ino, name, name_len, value, val_size, NULL, 0);

	do {
		reset_xattr_args(&xa);

		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, ig, &ino) 			?:
		      get_xattr(nfi, &txn, &ino, &xa);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	return ret ? ret : xa.val_size;
}

static int remove_xattr_wr(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg,
			   struct ngnfs_btree_op *op)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	if (!xattr)
		return -ENODATA;

	if (!xattr_names_equal(xattr->name, xattr->name_len, (u8 *) xa->name, xa->name_len))
		return NGNFS_BTREE_ITER_CONTINUE;

	xa->found = true;
	xa->names_delta = -(xa->name_len + 1);
	op->op = BOP_DELETE;

	return 0;
}

static int remove_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			struct xattr_args *xa)
{
	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;
	int ret;

	init_xattr_key(&key, xa->hash, 0);
	init_xattr_key(&last, xa->hash, U64_MAX);

	ret = ngnfs_btree_write_iter(nfi, txn, xa->ino->tblk, &xa->ino->ninode->xattrs, &key,
				     &last, remove_xattr_wr, xa);
	if (ret < 0)
		goto out;

	if (!xa->found)
		ret = -ENODATA;
out:
	return ret;
}

/*
 * Prevent storing so many xattrs we can't list the names in listxattr.
 */
static int check_xattr_names_len(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
				 struct ngnfs_inode_txn_ref *ino, int name_len)
{
	if ((le32_to_cpu(ino->ninode->xattr_names_len) + name_len + 1) > NGNFS_XATTR_MAX_NAMES_LEN)
		return -ERANGE;

	return 0;
}

/*
 * Update the inode record of total xattr names. name_len may be
 * negative.
 */
static int update_xattr_names_len(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
				  struct ngnfs_inode_txn_ref *ino, s32 name_len)
{
	s32 names_len = le32_to_cpu(ino->ninode->xattr_names_len);

	/* fs corruption and/or bugs can cause overflow/underflow/out of range */
	if (names_len > NGNFS_XATTR_MAX_NAMES_LEN ||
	    names_len + name_len < 0 ||
	    names_len + name_len > NGNFS_XATTR_MAX_NAMES_LEN)
		return -EUCLEAN;

	ngnfs_tblk_assign(ino->tblk, ino->ninode->xattr_names_len,
			  cpu_to_le32(names_len + name_len));

	return 0;
}

int ngnfs_xattr_remove(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name)
{
	struct ngnfs_transaction txn;
	struct ngnfs_inode_txn_ref ino;
	struct xattr_args xa;
	size_t name_len;
	int ret;

	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;

	ngnfs_txn_init(&txn);
	init_xattr_args(&xa, &ino, name, name_len, NULL, 0, NULL, 0);

	do {
		reset_xattr_args(&xa);

		ret = ngnfs_inode_get(nfi, &txn, NBF_WRITE, ig, &ino) 				?:
		      remove_xattr(nfi, &txn, &xa) 						?:
		      update_xattr_names_len(nfi, &txn, &ino, xa.names_delta);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	return ret;
}

/*
 * Three possibilities for set_xattr_wr callback:
 *
 * - XATTR_CREATE: set only if it does not already exist
 * - XATTR_REPLACE: set only if it already exists
 * - no flag: set whether or not it already exists
 */
static int set_xattr_wr(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg,
			struct ngnfs_btree_op *op)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	/* XATTR_REPLACE but no existing xattr */
	if (!xattr && xa->flags & XATTR_REPLACE)
		return NGNFS_BTREE_ITER_CONTINUE;

	/* XATTR_REPLACE or no flag, and xattr with matching hash exists */
	if (xattr && !(xa->flags & XATTR_CREATE)) {
		if (!xattr_names_equal(xattr->name, xattr->name_len,
				       (u8 *) xa->name, xa->name_len))
			return NGNFS_BTREE_ITER_CONTINUE;
	}

	/* XATTR_CREATE and xattr with matching hash exists */
	if (xattr && (xa->flags & XATTR_CREATE)) {
		if (xattr_names_equal(xattr->name, xattr->name_len,
				      (u8 *) xa->name, xa->name_len))
			return -EEXIST;
	}

	if (xattr) {
		op->op = BOP_REPLACE;
		op->key = *key;
	} else {
		op->op = BOP_INSERT;
		xa->names_delta = xa->name_len + 1;
		update_xattr_key(&op->key, xa->hash, xa->ino);
	}

	if (xa->flags & XATTR_REPLACE)
		xa->found = true; /* detect successful replace */

	op->val = xa->xattr;
	op->val_size = xa->xattr_size;

	return 0;
}

static int set_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		     struct xattr_args *xa)
{

	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;
	int ret;

	init_xattr_key(&key, xa->hash, 0);
	init_xattr_key(&last, xa->hash, U64_MAX);

	ret = ngnfs_btree_write_iter(nfi, txn, xa->ino->tblk, &xa->ino->ninode->xattrs, &key,
				     &last, set_xattr_wr, xa);
	if (ret < 0)
		goto out;

	if ((xa->flags & XATTR_REPLACE) && !xa->found)
		ret = -ENODATA;
out:
	return ret;
}

int ngnfs_xattr_set(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name,
		    void *value, size_t val_size, int flags)
{
	struct ngnfs_transaction txn;
	struct ngnfs_inode_txn_ref ino;
	struct xattr_args xa;
	struct ngnfs_xattr *xattr;
	size_t xa_size;
	size_t name_len;
	int ret;

	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;

	xa_size = xattr_size(name_len, val_size);
	if (xa_size > NGNFS_XATTR_MAX_SIZE)
		return -ERANGE;

	xattr = kmalloc(xa_size, GFP_NOFS);
	if (!xattr)
		return -ENOMEM;

	ngnfs_txn_init(&txn);
	init_xattr_args(&xa, &ino, name, name_len, value, val_size, xattr, flags);

	do {
		reset_xattr_args(&xa);

		ret = ngnfs_inode_get(nfi, &txn, NBF_WRITE, ig, &ino) 				?:
		      check_xattr_names_len(nfi, &txn, &ino, xa.name_len)			?:
		      set_xattr(nfi, &txn, &xa)							?:
		      update_xattr_names_len(nfi, &txn, &ino, xa.names_delta);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	kfree(xattr);

	return ret;
}

struct listxattr_args {
	char *buf;
	size_t size;
	size_t used;
};

/*
 * Copy one xattr name into the buf, followed by a null byte, unless the
 * buf is zero size, in which case just increment the size counter.
 */
static int fill_listxattr_rd(struct ngnfs_btree_key *key, void *val, size_t val_size, void *args)
{
	struct listxattr_args *la = args;
	struct ngnfs_xattr *xattr = val;
	size_t bytes;

	bytes = xattr->name_len + 1;
	if (la->size == 0) /* just counting the bytes, not copying them */
		goto out;

	if (bytes > (la->size - la->used))
		return -ERANGE;

	memcpy(la->buf + la->used, xattr->name, xattr->name_len);
	la->buf[la->used + xattr->name_len] = '\0';
out:
	la->used += bytes;

	/* check for file system corruption */
	if (la->used > NGNFS_XATTR_MAX_NAMES_LEN)
		return -EUCLEAN;

	return NGNFS_BTREE_ITER_CONTINUE;
}

/*
 * Return a list of names of extended attributes, separated by nulls, or
 * if the size of the buf is zero, the size that would be required to
 * return the list.
 */
int ngnfs_xattr_list(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, void *buf,
		     size_t size)
{
	struct ngnfs_transaction txn;
	struct ngnfs_btree_key key;
	struct ngnfs_inode_txn_ref ino;
	struct listxattr_args la;
	int ret;

	la.buf = buf;
	la.size = size;

	ngnfs_txn_init(&txn);
	init_xattr_key(&key, 0, 0);

	do {
		la.used = 0;

		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, ig, &ino)				?:
		      ngnfs_btree_read_iter(nfi, &txn, &ino.ninode->xattrs, &key,
					    NULL, NULL, fill_listxattr_rd, &la);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	return ret ?: la.used;
}
