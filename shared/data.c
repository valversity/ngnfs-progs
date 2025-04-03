/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/math64.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/data.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"

/*
 * File data is stored in simple tree of indirect blocks with data all
 * at the same level of the tree. The topmost block in the tree and its
 * level are stored in the inode. The tree is sparse and branches are
 * grown as necessary to index newly written data blocks. While
 * manipulating the tree, we use levels to identify the blocks at
 * various levels of the tree, with the highest levels closer to the
 * root.
 *
 * The data root field in the inode contains both the persistent
 * reference (block number, etc.) to the root block of the tree, plus
 * the height of the tree (the level of the block it points to, plus 1).
 * The root block reference is not part of an indirect block.
 *
 * The level of a block is:
 *
 * 0 = data block
 * 1 = references to data blocks
 * 2 = references to single indirect blocks (pointing to data blocks)
 * 3 = references to a double indirect block
 * 4 = pointer to a triple indirect block
 *
 * Thus a data root reference with height 1 points to a block of level 0
 * = a single block of data at logical file offset 0.
 *
 * The contents of file data and indirect blocks are read/written using
 * structs containing both the transaction block reference and a pointer
 * to the contents in memory.
 */
struct data_txn_ref {
	struct ngnfs_txn_block *tblk;
	void *buf;
};

struct indirect_txn_ref {
	struct ngnfs_txn_block *tblk;
	struct ngnfs_indirect_block *iblk;
};

/*
 * Return the logical block number containing offset within a file.
 */
static u64 dblk_from_offset(u64 offset)
{
	return offset >> NGNFS_BLOCK_SHIFT;
}

/*
 * Return the offset past the beginning of a block.
 */
static u64 offset_in_blk(u64 offset)
{
	return offset & (NGNFS_BLOCK_SIZE - 1ULL);
}

/*
 * Calculate the index of the block reference for this logical block
 * within an indirect block at this level (1 = pointers to data blocks).
 */
static u32 calc_ref_ind(u64 dblk, int level)
{
	u32 ind;
	int i;

	BUG_ON(level < 1);

	for (i = 1; i <= level; i++)
		dblk = div_u64_rem(dblk, NGNFS_DATA_REFS_PER_BLOCK, &ind);

	return ind;
}

/*
 * Calculate the height of the tree (level of the root pointer) needed
 * to index the logical block dblk in this file.
 */
static u8 height_from_dblk(u64 dblk)
{
	u64 total = NGNFS_DATA_REFS_PER_BLOCK;
	u8 height = 2;

	if (dblk == 0)
		return 1;

	while (dblk >= total) {
		height++;
		total *= NGNFS_DATA_REFS_PER_BLOCK;
	}

	return height;
}

/*
 * Information for finding and allocating file data and the indirect
 * blocks along the way to a logical file block.
 */
struct data_path_args {
	struct ngnfs_inode_txn_ref ino;
	struct data_txn_ref data;
	u64 offset;
	u64 dblk;
	void *buf;
	size_t len;
	int write;
	nbf_t nbf; 	/* initial read/write mode for inode and data block */
	u8 level; 	/* number of indirect blocks necessary to get this block */
};

static void init_data_path_args(struct data_path_args *da, u64 offset, void *buf, size_t len,
				int write)
{
	da->ino.ninode = NULL;
	da->ino.tblk = NULL;
	da->data.tblk = NULL;
	da->data.buf = NULL;
	da->offset = offset;
	da->dblk = dblk_from_offset(offset);
	da->buf = buf;
	da->len = len;
	da->write = write;
	da->nbf = write ? NBF_WRITE : NBF_READ;
	da->level = height_from_dblk(da->dblk);
}

static int alloc_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct ngnfs_block_ref *ref, struct ngnfs_txn_block **tblk_ret,
		       void **data_ret)
{
	u64 bnr;
	int ret;

	ret = ngnfs_txn_alloc_meta(txn, &bnr);
	if (ret < 0)
		goto out;

	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE | NBF_NEW, tblk_ret, data_ret);
	if (ret < 0)
		goto out;

	ref->bnr = cpu_to_le64(bnr);
	ref->alloc_counter = 0; /* XXX */
out:
	return ret;
}

/*
 * Fill in a file data branch for logical file block dblk from
 * start_level to child_level. If child doesn't exist, alloc a data
 * block. If child does exist, graft it in. Return the top of the
 * branch, and the data block if we allocated it.
 */
static int fill_block_path(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			   u64 dblk, u8 start_level, u8 child_level,
			   struct ngnfs_block_ref *child, struct ngnfs_block_ref *branch_ret,
			   struct data_txn_ref *data_ret)
{
	struct indirect_txn_ref parent;
	struct ngnfs_block_ref branch;
	struct ngnfs_block_ref ref;
	struct ngnfs_txn_block *tblk;
	void *buf;
	u8 level;
	u8 ind;
	int ret;

	BUG_ON(start_level < 1);

	branch.bnr = 0;
	branch.alloc_counter = 0;

	parent.tblk = NULL;
	parent.iblk = NULL;

	level = start_level;
	while(level > child_level) {
		ret = alloc_block(nfi, txn, &ref, &tblk, &buf);
		if (ret < 0)
			goto out;

		if (!branch.bnr)
			branch = ref;

		if (parent.tblk) {
			ind = calc_ref_ind(dblk, level);
			ngnfs_tblk_assign(parent.tblk, parent.iblk->refs[ind], ref);
		}
		parent.tblk = tblk;
		parent.iblk = buf;
		level--;
	}

	if (child_level == 0) {
		data_ret->tblk = tblk;
		data_ret->buf = buf;
	} else {
		ind = calc_ref_ind(dblk, child_level);
		ngnfs_tblk_assign(parent.tblk, parent.iblk->refs[ind], *child);
	}

	*branch_ret = branch;
	ret = 0;
out:
	return ret;
}

/*
 * Grow the height of the tree to the height necessary to store
 * da->dblk. If the tree is empty, allocate a data block and return it
 * in da->data.
 */
static int grow_height(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct data_path_args *da)
{
	struct ngnfs_inode_txn_ref *ino;
	struct ngnfs_block_ref new_root;
	struct ngnfs_data_root *dr;
	u64 bnr;
	u64 dblk;
	int ret;

	ino = &da->ino;
	bnr = le64_to_cpu(ino->ninode->ig.ino); /* XXX should get out of tblk */
	dr = &ino->ninode->data;

	/* attempt to convert read access on inode to write */
	if (da->nbf != NBF_WRITE) {
		ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE, NULL, NULL);
		if (ret < 0)
			goto out;
	}

	/* an existing tree will index to dblk 0 in new levels */
	if (dr->height != 0)
		dblk = 0;
	else
		dblk = da->dblk;

	ret = fill_block_path(nfi, txn, dblk, da->level, dr->height, &dr->ref, &new_root,
			      &da->data);
	if (ret < 0)
		goto out;

	ngnfs_tblk_assign(ino->tblk, dr->ref, new_root);
	ngnfs_tblk_assign(ino->tblk, dr->height, da->level);
out:
	return ret;
}

/*
 * Grow a branch for da->dblk from the parent's level down through the
 * data block, graft it into the existing tree, and return the data
 * block in da->data. Cannot be used at the root of the tree.
 */
static int grow_branch(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct indirect_txn_ref *parent, u64 bnr, u8 level,
		       struct data_path_args *da)
{
	struct ngnfs_block_ref branch;
	u8 ind;
	int ret;

	BUG_ON(da->ino.ninode->data.height == level);

	/* we have read access on the parent, try for write access */
	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE, NULL, NULL);
	if (ret < 0)
		goto out;

	/* goal level is a data block, which is level 0 */
	ret = fill_block_path(nfi, txn, da->dblk, level, 0, NULL, &branch, &da->data);
	if (ret < 0)
		goto out;

	ind = calc_ref_ind(da->dblk, level);
	ngnfs_tblk_assign(parent->tblk, parent->iblk->refs[ind], branch);
out:
	return ret;
}

/*
 * Get the block at this logical file data offset and return it in
 * da->data. If it doesn't exist, allocate it and all indirect blocks on
 * its path if necessary. This is only called after checking that a read
 * is from a valid range of the file, so it should always allocate
 * missing blocks to zero-fill holes.
 */
static int get_data_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			  struct data_path_args *da)
{
	struct ngnfs_data_root *dr;
	struct indirect_txn_ref parent;
	struct ngnfs_txn_block *tblk;
	void *buf;
	nbf_t nbf;
	u64 bnr;
	u8 level;
	u8 ind;
	int ret;

	dr = &da->ino.ninode->data;

	/*
	 * Grow height of tree as necessary. If we grow a tree with one
	 * data block (height = 1), then we also get the data block.
	 */
	if (da->level > dr->height) {
		ret = grow_height(nfi, txn, da);
		if (ret < 0)
			goto out;

		if (da->level == 1)
			goto out;
	}

	nbf = NBF_READ;
	level = dr->height;
	bnr = le64_to_cpu(dr->ref.bnr);

	while (level-- > 0) {
		if (level == 0)
			nbf = da->nbf;

		ret = ngnfs_txn_get_block(nfi, txn, bnr, nbf, &tblk,
					  (void **) &buf);
		if (ret < 0)
			goto out;

		if (level == 0) {
			da->data.tblk = tblk;
			da->data.buf = buf;
			goto out;
		}

		/* look up next block reference */
		parent.tblk = tblk;
		parent.iblk = buf;
		ind = calc_ref_ind(da->dblk, level);
		bnr = le64_to_cpu(parent.iblk->refs[ind].bnr);

		if (bnr == 0) {
			ret = grow_branch(nfi, txn, &parent, bnr, level, da);
			if (ret < 0)
				goto out;
			break;
		}
	};
out:
	BUG_ON((ret == 0) && (da->data.buf == NULL));
	return ret;
}

static int read_write_block(struct data_txn_ref *dtref, size_t offset, void *buf, size_t len,
			    int write)
{
	BUG_ON(offset >= NGNFS_BLOCK_SIZE);

	if (write)
		ngnfs_tblk_memcpy(dtref->tblk, dtref->buf + offset, buf, len);
	else
		memcpy(buf, dtref->buf + offset, len);

	return 0;
}

/*
 * After reading the inode, trim the requested file data range for a
 * read request to only cover reads less than the size of the file. If
 * the range doesn't overlap any allocated part of the file, set len to
 * 0 and the caller will return no error.
 */
static void trim_read_range(struct data_path_args *da)
{
	size_t i_size;

	i_size = le64_to_cpu(da->ino.ninode->size);

	if (da->offset >= i_size)
		da->len = 0;
	else if ((da->offset + da->len) >= i_size)
		da->len = i_size - da->offset;
}

/*
 * If file size needs to be updated, then we already have write access
 * to the inode, so this currently can't fail.
 */
static void update_isize(struct data_path_args *da)
{
	struct ngnfs_inode_txn_ref *ino = &da->ino;

	if (da->offset > le64_to_cpu(ino->ninode->size))
		ngnfs_tblk_assign(ino->tblk, ino->ninode->size, cpu_to_le64(da->offset));
}

static int read_write_range(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			    struct data_path_args *da, size_t *bytes)
{
	size_t start, bytes_done, bytes_todo;
	int ret;

	if (!da->write)
		trim_read_range(da);

	bytes_done = 0;
	start = offset_in_blk(da->offset);

	while (bytes_done < da->len) {
		ret = get_data_block(nfi, txn, da);
		if (ret < 0)
			goto out;

		bytes_todo = da->len - bytes_done;
		if (bytes_todo > (NGNFS_BLOCK_SIZE - start))
			bytes_todo = NGNFS_BLOCK_SIZE - start;

		ret = read_write_block(&da->data, start, da->buf + bytes_done, bytes_todo,
				       da->write);
		if (ret < 0)
			goto out;

		bytes_done += bytes_todo;
		da->offset += bytes_todo;
		da->dblk = dblk_from_offset(da->offset);
		da->level = height_from_dblk(da->dblk);
		start = 0;
	}

	ret = 0;
out:
	if (bytes_done) {
		*bytes = bytes_done;
		update_isize(da);
		ret = 0; /* any error will be returned on next call */
	}
	return ret;
}

static ssize_t read_write_data(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			       u64 offset, void *buf, size_t len, int write)
{
	struct {
		struct data_path_args da;
		struct ngnfs_transaction txn;
	} *op;
	size_t bytes;
	int ret;

	op = kmalloc(sizeof(*op), GFP_NOFS);
	if (!op)
		return -ENOMEM;

	bytes = 0;
	init_data_path_args(&op->da, offset, buf, len, write);
	ngnfs_txn_init(&op->txn);

	/* XXX break up into smaller transactions */
	do {
		ret = ngnfs_inode_get(nfi, &op->txn, op->da.nbf, ig, &op->da.ino) 		?:
		      read_write_range(nfi, &op->txn, &op->da, &bytes);

	} while (ngnfs_txn_retry(nfi, &op->txn, &ret));

	ngnfs_txn_teardown(nfi, &op->txn);

	if (ret == 0)
		ret = bytes;

	return ret;
}

ssize_t ngnfs_data_write(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			 u64 offset, void *buf, size_t len)
{
	return read_write_data(nfi, ig, offset, buf, len, 1);
}

ssize_t ngnfs_data_read(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			u64 offset, void *buf, size_t len)
{
	return read_write_data(nfi, ig, offset, buf, len, 0);
}
