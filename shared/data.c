/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
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
 * 3 = references to double indirect blocks
 * 4 = references to triple indirect blocks
 *
 * Thus a data root reference with height 1 points to a block of level 0
 * = a single block of data at logical file offset 0.
 */

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
	return offset & NGNFS_BLOCK_MASK;
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

	for (i = 1; i < level; i++)
		dblk >>= NGNFS_DATA_REFS_PER_BLK_SHIFT;

	ind = dblk & (NGNFS_DATA_REFS_PER_BLK - 1ULL);

	return ind;
}

/*
 * Calculate the height of the tree (level of the root pointer) needed
 * to index the logical block dblk in this file.
 */
static u8 height_from_dblk(u64 dblk)
{
	u8 height = 2;

	if (dblk == 0)
		return 1;

	while (dblk >>= NGNFS_DATA_REFS_PER_BLK_SHIFT)
		height++;

	return height;
}

static int alloc_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct ngnfs_txn_block *parent_tblk, struct ngnfs_block_ref *parent_ref,
		       struct ngnfs_txn_block **tblk_ret, void **data_ret)
{
	struct ngnfs_block_ref ref;
	u64 bnr;
	int ret;

	ret = ngnfs_txn_alloc_meta(txn, &bnr);
	if (ret < 0)
		goto out;

	ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_WRITE | NBF_NEW, tblk_ret, data_ret);
	if (ret < 0)
		goto out;

	ref.bnr = cpu_to_le64(bnr);
	ref.alloc_counter = 0; /* XXX */
	ngnfs_tblk_assign(parent_tblk, *parent_ref, ref);
out:
	return ret;
}

static int grow_height(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		       struct ngnfs_inode_txn_ref *ino, int height)
{
	struct ngnfs_txn_block *parent_tblk;
	struct ngnfs_block_ref *parent_ref;
	struct ngnfs_indirect_block *iblk;
	struct ngnfs_txn_block *tblk;
	struct ngnfs_data_root *dr;
	struct ngnfs_block_ref ref;
	u8 level;
	int ret;

	dr = &ino->ninode->data;

	ret = 0;
	if ((dr->height > 0) &&
	    (height > dr->height)) {
		/* start at root and fill in till we get to existing tree */
		level = height;
		ref = dr->ref; /* save current root of tree */
		parent_tblk = ino->tblk;
		parent_ref = &dr->ref;

		while (level-- > dr->height) {
			ret = alloc_block(nfi, txn, parent_tblk, parent_ref,
					  &tblk, (void **) &iblk);
			if (ret < 0)
				goto out;
			/* growing height will always index old data to 0 */
			parent_tblk = tblk;
			parent_ref = &iblk->refs[0];
		}

		/* now put existing tree in indirect block */
		ngnfs_tblk_assign(tblk, iblk->refs[0], ref);
		ngnfs_tblk_assign(ino->tblk, dr->height, height);
	}
out:
	return ret;
}

/*
 * Allocate the indirect blocks for file data offset if they aren't
 * already, and return the parent block of the data at offset. This is
 * only called after checking that a read is from a valid range of the
 * file. If a read hits an unallocated range, this returns 0 for the
 * number of refs and the caller will zero fill the buffer.
 */
static int get_data_block_parent(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
				 struct ngnfs_inode_txn_ref *ino, u64 offset, int write,
				 struct ngnfs_txn_block **tblkp,
				 struct ngnfs_block_ref **refs, int *nr)
{
	struct ngnfs_txn_block *parent_tblk;
	struct ngnfs_block_ref *parent_ref;
	struct ngnfs_indirect_block *iblk;
	struct ngnfs_txn_block *tblk;
	struct ngnfs_data_root *dr;
	u64 dblk;
	u64 bnr;
	u8 height;
	u8 level;
	u8 ind;
	int ret;

	dr = &ino->ninode->data;
	dblk = dblk_from_offset(offset);
	height = height_from_dblk(dblk);

	/* special case: dblk 0 and tree with 0 or 1 blocks in it */
	if ((height == 1) &&
	    (dr->height <= 1))  {
		if (dr->height == 0) {
			ret = alloc_block(nfi, txn, ino->tblk, &dr->ref, NULL, NULL);
			if (ret < 0)
				goto out;
			ngnfs_tblk_assign(ino->tblk, dr->height, height);
		}

		*tblkp = ino->tblk;
		*refs = &dr->ref;
		*nr = 1;
		ret = 0;
		goto out;
	}

	/* grow the height of any existing data */
	ret = grow_height(nfi, txn, ino, height);
	if (ret < 0)
		goto out;

	/* two cases: totally empty tree and tree of necessary height */
	level = dr->height ? dr->height : height;
	bnr = le64_to_cpu(dr->ref.bnr);
	parent_tblk = ino->tblk;
	parent_ref = &dr->ref;

	/* may still have totally empty tree */
	while (level-- > 1) {
		if (bnr) {
			ret = ngnfs_txn_get_block(nfi, txn, bnr, NBF_READ, &tblk, (void **) &iblk);
			if (ret < 0)
				goto out;

		} else {
			if (!write) {
				*tblkp = NULL;
				*refs = NULL;
				*nr = NGNFS_DATA_REFS_PER_BLK - calc_ref_ind(dblk, 1);
			}

			/* get write access to update parent with new block */
			if (parent_ref) {
				ret = ngnfs_txn_get_block(nfi, txn, le64_to_cpu(parent_ref->bnr),
							  NBF_WRITE, NULL, NULL);
				if (ret < 0)
					goto out;
			}

			ret = alloc_block(nfi, txn, parent_tblk, parent_ref,
					  &tblk, (void **) &iblk);
			if (ret < 0)
				goto out;
		}

		if (level == 0)
			break;

		/* look up next block reference */
		ind = calc_ref_ind(dblk, level);
		bnr = le64_to_cpu(iblk->refs[ind].bnr);

		parent_tblk = tblk;
		parent_ref = &iblk->refs[ind];
	};

	if (dr->height < height)
		ngnfs_tblk_assign(ino->tblk, dr->height, height);

	*tblkp = tblk;
	ind = calc_ref_ind(dblk, 1);
	*refs = &iblk->refs[ind];
	*nr = NGNFS_DATA_REFS_PER_BLK - ind;
out:
	return ret;
}

static int read_write_block(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			    struct ngnfs_txn_block *parent_tblk,
			    struct ngnfs_block_ref *parent_refs, int ind,
			    int start, void *buf, int len, int write)
{
	struct ngnfs_txn_block *tblk;
	char *data;
	u64 bnr;
	nbf_t nbf;
	int ret;

	BUG_ON(start + len > NGNFS_BLOCK_SIZE);

	if (parent_refs)
		bnr = le64_to_cpu(parent_refs[ind].bnr);
	else
		bnr = 0;

	/* zero fill sparse blocks */
	if (!write && (bnr == 0)) {
		memset(buf, 0, len);
		ret = 0;
		goto out;
	}

	if (bnr) {
		nbf = write ? NBF_WRITE : NBF_READ;
		ret = ngnfs_txn_get_block(nfi, txn, bnr, nbf, &tblk, (void **) &data);
		if (ret < 0)
			goto out;

	} else {
		ret = alloc_block(nfi, txn, parent_tblk, &parent_refs[ind], &tblk, (void **) &data);
		if (ret < 0)
			goto out;
	}

	if (write)
		ngnfs_tblk_memcpy(tblk, data + start, buf, len);
	else
		memcpy(buf, data + start, len);
out:
	return ret;
}

/*
 * After reading the inode, trim the requested file data range for a
 * read request to only cover reads less than the size of the file. If
 * the range doesn't overlap any allocated part of the file, set len to
 * 0 and the caller will return no error.
 */
static void trim_read_range(struct ngnfs_inode *ino, u64 offset, size_t *len)
{
	u64 i_size;

	i_size = le64_to_cpu(ino->size);

	if (offset >= i_size)
		*len = 0;
	else if ((offset + *len) >= i_size)
		*len = i_size - offset;
}

/*
 * If file size needs to be updated, then we already have write access
 * to the inode, so this currently can't fail.
 */
static void update_isize(struct ngnfs_inode_txn_ref *itref, u64 size)
{
	if (size > le64_to_cpu(itref->ninode->size))
		ngnfs_tblk_assign(itref->tblk, itref->ninode->size, cpu_to_le64(size));
}

static int read_write_range(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
			    struct ngnfs_inode_txn_ref *itref, u64 offset, char *buf, size_t len,
			    int write, size_t *bytes)
{
	struct ngnfs_txn_block *tblk;
	struct ngnfs_block_ref *refs;
	size_t start, done, todo;
	int nr;
	int i;
	int ret;

	if (!write)
		trim_read_range(itref->ninode, offset, &len);

	done = 0;
	start = offset_in_blk(offset);

	while (done < len) {
		ret = get_data_block_parent(nfi, txn, itref, offset, write, &tblk, &refs, &nr);
		if (ret < 0)
			goto out;

		for (i = 0; i < nr; i++) {
			todo = len - done;
			if (todo > (NGNFS_BLOCK_SIZE - start))
				todo = NGNFS_BLOCK_SIZE - start;

			ret = read_write_block(nfi, txn, tblk, refs, i, start,
					       buf + done, todo, write);
			if (ret < 0)
				goto out;

			offset += todo;
			done += todo;
			start = 0;

			if (done >= len)
				break;
		}
	}

	*bytes = done;
	ret = 0;
out:
	if (done) {
		if (write)
			update_isize(itref, offset);
		ret = 0; /* any error will be returned on next call */
	}

	return ret;
}

static ssize_t read_write_data(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			       u64 offset, void *buf, size_t len, int write)
{
	struct ngnfs_transaction txn;
	struct ngnfs_inode_txn_ref itref;
	size_t start, bytes, done, todo;
	nbf_t nbf;
	int ret;

	bytes = 0;
	start = offset_in_blk(offset);
	nbf = write ? NBF_WRITE : NBF_READ;

	ngnfs_txn_init(&txn);

	/* split into appropriate-sized transactions */
	for (done = 0; done < len; done += bytes) {

		do {
			todo = len - done;
			if (todo > (NGNFS_DATA_MAX_IO - start))
				todo = NGNFS_DATA_MAX_IO - start;

			/* XXX need to prevent other IOs interleaving */
			ret = ngnfs_inode_get(nfi, &txn, nbf, ig, &itref)			?:
			      read_write_range(nfi, &txn, &itref, offset, buf + done,
					       todo, write, &bytes);

		} while (ngnfs_txn_retry(nfi, &txn, &ret));

		ngnfs_txn_teardown(nfi, &txn);

		if (ret < 0)
			goto out;

		if (bytes == 0)
			break;

		offset += bytes;
		start = 0;
	}

	ret = done;
out:
	return ret ?: done;
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
