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
 * Maximum size of both xattr name and value added together.
 *
 * TODO: support much larger xattrs by storing in blocks instead of
 * btree items.
 */
#define NGNFS_XATTR_MAX_SIZE NGNFS_BTREE_MAX_VAL_SIZE

static u64 xattr_hash(void *name, size_t name_len)
{
	/* XXX using the same #defines as dirents, should be different? */
	return xxh64(name, name_len, NGNFS_DIRENT_HASH_SEED) & NGNFS_DIRENT_HASH_MASK;
}

struct xattr_args {
	struct ngnfs_inode_txn_ref *ino;
	u64 hash;
	char *name;
	char *value;
	struct ngnfs_xattr *xattr;
	size_t name_len;
	size_t val_size;
	size_t xa_size;
	int flags;
	bool found;
};

static size_t xattr_size(size_t name_len, size_t val_size) {
	return offsetof(struct ngnfs_xattr, name) + name_len + val_size;
}

static void init_xattr_args(struct xattr_args *xa, struct ngnfs_inode_txn_ref *ino,
			    char *name, size_t name_len, void *value, size_t val_size,
			    struct ngnfs_xattr *xattr, int flags)
{
	xa->ino = ino;
	xa->hash = xattr_hash(name, name_len);
	xa->name = name;
	xa->value = value;
	xa->xattr = xattr;
	xa->name_len = name_len;
	xa->val_size = val_size;
	xa->found = false;
	xa->flags = flags;

	if (xattr) {
		xa->xa_size = xattr_size(name_len, val_size);
		xattr->name_len = name_len;
		xattr->val_len = cpu_to_le16(val_size);
		memcpy(xattr->name, name, name_len);
		memcpy(xattr->name + name_len, value, val_size);
	} else {
		xa->xa_size = 0;
	}

}

static void init_xattr_key(struct ngnfs_btree_key *key, u64 hash)
{
	*key = (struct ngnfs_btree_key) {
		.k[0] = cpu_to_le64(hash),
	};
}

static bool xattr_names_equal(u8 *a, size_t a_len, u8 *b, size_t b_len)
{
	return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static int fill_xattr_rd(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	if (!xattr_names_equal(xattr->name, xattr->name_len, (u8 *) xa->name, xa->name_len))
		return NGNFS_BTREE_ITER_CONTINUE;

	if (le16_to_cpu(xattr->val_len) > xa->val_size)
		return -ENOBUFS;

	memcpy(xa->value, xattr->name + xattr->name_len, le16_to_cpu(xattr->val_len));
	xa->val_size = le16_to_cpu(xattr->val_len);
	xa->found = true;

	return 0;
}

static int get_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		     struct ngnfs_inode_txn_ref *ino, struct xattr_args *xa)
{
	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;
	int ret;

	init_xattr_key(&key, xa->hash);
	init_xattr_key(&last, xa->hash | NGNFS_DIRENT_COLL_BIT);

	ret = ngnfs_btree_read_iter(nfi, txn, &ino->ninode->xattrs, &key, NULL, &last,
				    fill_xattr_rd, xa);

	if (ret < 0)
		goto out;

	if (!xa->found)
		ret = -ENODATA;
out:
	return ret;
}

ssize_t ngnfs_xattr_get(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig, char *name,
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
		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, ig, &ino) 			?:
		      get_xattr(nfi, &txn, &ino, &xa);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	if (ret == 0)
		ret = xa.val_size;

	return ret;
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

	op->delete = 1;
	xa->found = true;

	return 0;
}

static int remove_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			struct xattr_args *xa)
{
	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;
	int ret;

	init_xattr_key(&key, xa->hash);
	init_xattr_key(&last, xa->hash | NGNFS_DIRENT_COLL_BIT);

	ret = ngnfs_btree_write_iter(nfi, txn, xa->ino->tblk, &xa->ino->ninode->xattrs, &key,
				     &last, remove_xattr_wr, xa);

	if ((ret == 0) && (!xa->found))
		ret = -ENODATA;

	return ret;
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
		ret = ngnfs_inode_get(nfi, &txn, NBF_WRITE, ig, &ino) 			?:
		      remove_xattr(nfi, &txn, &xa);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	return ret;
}

static int insert_xattr_wr(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg,
			   struct ngnfs_btree_op *op)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	if (xattr) {
		if (xattr_names_equal(xattr->name, xattr->name_len, (u8 *) xa->name, xa->name_len))
			return -EEXIST;

		if (xa->hash == le64_to_cpu(key->k[0])) {
			if (xa->hash & NGNFS_DIRENT_COLL_BIT)
				return -ENOSPC;
			xa->hash |= NGNFS_DIRENT_COLL_BIT;
			return NGNFS_BTREE_ITER_CONTINUE;
		}
	}

	op->insert = 1;
	op->val = xa->xattr;
	op->val_size = xa->xa_size;
	init_xattr_key(&op->key, xa->hash);

	return 0;
}

static int insert_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			struct xattr_args *xa)
{

	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;

	init_xattr_key(&key, xa->hash);
	init_xattr_key(&last, xa->hash | NGNFS_DIRENT_COLL_BIT);

	return ngnfs_btree_write_iter(nfi, txn, xa->ino->tblk, &xa->ino->ninode->xattrs, &key,
				      &last, insert_xattr_wr, xa);
}

static int replace_xattr_wr(struct ngnfs_btree_key *key, void *val, size_t val_size, void *arg,
			    struct ngnfs_btree_op *op)
{
	struct xattr_args *xa = arg;
	struct ngnfs_xattr *xattr = val;

	if (!xattr)
		return -ENODATA;

	/* check for hash collision, retry once, then give up */
	if (!xattr_names_equal(xattr->name, xattr->name_len, (u8 *) xa->name, xa->name_len)) {
		if (xa->hash == le64_to_cpu(key->k[0])) {
			if (xa->hash & NGNFS_DIRENT_COLL_BIT)
				return -ENOSPC;
			xa->hash |= NGNFS_DIRENT_COLL_BIT;
			return NGNFS_BTREE_ITER_CONTINUE;
		}
	}

	/* replace is done by setting both delete and insert */
	op->delete = 1;
	op->insert = 1;
	op->val = xa->xattr;
	op->val_size = xa->xa_size;
	xa->found = 1;

	return 0;
}

static int replace_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			 struct xattr_args *xa)
{

	struct ngnfs_btree_key key;
	struct ngnfs_btree_key last;
	int ret;

	init_xattr_key(&key, xa->hash);
	init_xattr_key(&last, xa->hash | NGNFS_DIRENT_COLL_BIT);

	ret = ngnfs_btree_write_iter(nfi, txn, xa->ino->tblk, &xa->ino->ninode->xattrs, &key,
				     &last, replace_xattr_wr, xa);

	if (!xa->found)
		ret = -ENODATA;

	return ret;
}

/*
 * Use the most efficient btree operation to implement setxattr
 * depending on which flags are set. Replace means only set if it
 * exists, create means only set if it does NOT exist, and no flags mean
 * set it regardless of whether it exists.
 */
static int set_xattr(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		     struct xattr_args *xa)
{
	int ret;

	if (xa->flags & XATTR_REPLACE)
		ret = replace_xattr(nfi, txn, xa);
	else if (xa->flags & XATTR_CREATE)
		ret = insert_xattr(nfi, txn, xa);
	else {
		ret = remove_xattr(nfi, txn, xa);
		if (ret < 0 && ret != -ENODATA)
			goto out;
		ret = insert_xattr(nfi, txn, xa);
	}
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

	init_xattr_args(&xa, &ino, name, name_len, value, val_size, xattr, flags);

	ngnfs_txn_init(&txn);

	do {
		ret = ngnfs_inode_get(nfi, &txn, NBF_WRITE, ig, &ino) 			?:
		      set_xattr(nfi, &txn, &xa);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	kfree(xattr);
	return ret;
}
