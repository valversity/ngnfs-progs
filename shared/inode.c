/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/minmax.h"
#include "shared/lk/stat.h"
#include "shared/lk/types.h"

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/txn.h"

/*
 * For now inode structs are simply stored at the front of blocks.  The
 * block number is the inode number, avoiding false sharing of block
 * access for consecutive inodes.  We're almost certainly going to want
 * to pack inode data into the rest of the block.
 */

/*
 * Most (all?) file systems have a non-zero i_size for a dir containing
 * only "." and "..", so define one that matches how we count i_size for
 * other dentries (name_len plus null terminator). It doesn't really
 * matter.
 */
#define NGNFS_DIR_SIZE	5

int ngnfs_inode_init(struct ngnfs_inode_txn_ref *itref, struct ngnfs_inode_ino_gen *ig, u32 nlink,
		     umode_t mode, u64 nsec, struct ngnfs_inode_ino_gen *parent_ig)
{
	struct ngnfs_txn_block *tblk = itref->tblk;
	struct ngnfs_inode *ninode = itref->ninode;
	u64 i_size;

	if (S_ISDIR(mode))
		i_size = NGNFS_DIR_SIZE;
	else
		i_size = 0;

	ngnfs_tblk_assign(tblk, ninode->ig.ino, cpu_to_le64(ig->ino));
	ngnfs_tblk_assign(tblk, ninode->ig.gen, cpu_to_le64(ig->gen));
	ngnfs_tblk_assign(tblk, ninode->size, cpu_to_le64(i_size));
	ngnfs_tblk_assign(tblk, ninode->version, cpu_to_le64(1));
	ngnfs_tblk_assign(tblk, ninode->parent_ig.ino, cpu_to_le64(parent_ig->ino));
	ngnfs_tblk_assign(tblk, ninode->parent_ig.gen, cpu_to_le64(parent_ig->gen));
	ngnfs_tblk_assign(tblk, ninode->nlink, cpu_to_le32(nlink));
	ngnfs_tblk_assign(tblk, ninode->uid, 0);
	ngnfs_tblk_assign(tblk, ninode->gid, 0);
	ngnfs_tblk_assign(tblk, ninode->mode, cpu_to_le32(mode));
	ngnfs_tblk_assign(tblk, ninode->rdev, 0);
	ngnfs_tblk_assign(tblk, ninode->flags, 0);
	ngnfs_tblk_assign(tblk, ninode->xattr_creates, 0);
	ngnfs_tblk_assign(tblk, ninode->xattr_names_len, 0);
	ngnfs_tblk_assign(tblk, ninode->atime_nsec, cpu_to_le64(nsec));
	ngnfs_tblk_assign(tblk, ninode->ctime_nsec, ninode->atime_nsec);
	ngnfs_tblk_assign(tblk, ninode->mtime_nsec, ninode->atime_nsec);
	ngnfs_tblk_assign(tblk, ninode->crtime_nsec, ninode->atime_nsec);
	ngnfs_tblk_memset(tblk, &ninode->dirents, 0, sizeof(ninode->dirents));
	ngnfs_tblk_memset(tblk, &ninode->xattrs, 0, sizeof(ninode->xattrs));

	return 0;
}

/*
 * XXX should be validating that the block is a valid inode, etc.
 */
int ngnfs_inode_get(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, nbf_t nbf,
		    struct ngnfs_inode_ino_gen *ig, struct ngnfs_inode_txn_ref *itref)
{
	if (ig->ino == 0) /* probably a buggy caller but could be corruption */
		return -EUCLEAN;

	return ngnfs_txn_get_block(nfi, txn, ig->ino, nbf, &itref->tblk, (void **)&itref->ninode);
}

int ngnfs_inode_alloc(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		      struct ngnfs_inode_ino_gen *ig, struct ngnfs_inode_txn_ref *itref)
{
	struct ngnfs_inode_ino_gen new;
	int ret;

	new.gen = 1; /* XXX should look up previous gen and increment */
	ret = ngnfs_txn_alloc_meta(txn, &new.ino) ?:
	      ngnfs_inode_get(nfi, txn, NBF_WRITE | NBF_NEW, &new, itref);

	if (ret == 0)
		*ig = new;

	return ret;
}

/*
 * A transaction that just copies the inode in the block to the caller's
 * buffer.
 */
int ngnfs_inode_read_copy(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *ig,
			  void *buf, int size)
{
	struct ngnfs_inode_txn_ref itref;
	struct ngnfs_transaction txn;
	int ret;

	if (WARN_ON_ONCE(size < 0)) {
		ret = -EINVAL;
		goto out;
	}

	ngnfs_txn_init(&txn);

	do {
		ret = ngnfs_inode_get(nfi, &txn, NBF_READ, ig, &itref);
		if (ret == 0) {
			ret = min(size, sizeof(struct ngnfs_inode));
			memcpy(buf, itref.ninode, ret);
		}
	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);
out:
	return ret;
}

/*
 * Update an inode to reflect the addition or removal of one or more
 * links to it.
 */
int ngnfs_inode_update(struct ngnfs_txn_block *tblk, struct ngnfs_inode *inode, s32 delta)
{
	s32 nlink = le32_to_cpu(inode->nlink);

	if ((delta > 0) && (nlink > NGNFS_LINK_MAX - delta))
		return -EMLINK;

	/* nlink < 0 is a data corruption bug */
	if ((delta < 0) && (nlink + delta < 0))
		return -EUCLEAN;

	/*
	 * If this is the removal of the last external link to a dir, remove the "."
	 * self-link too.
	 */
	if ((nlink == 2 && delta == -1) && ((le32_to_cpu(inode->mode) & S_IFMT) == S_IFDIR))
		delta = -2;

	ngnfs_tblk_assign(tblk, inode->nlink, cpu_to_le32(nlink + delta));

	return 0;
}
