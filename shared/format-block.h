/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_BLOCK_H
#define NGNFS_SHARED_FORMAT_BLOCK_H

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/limits.h"
#include "shared/lk/types.h"

#define NGNFS_BLOCK_SHIFT	12
#define NGNFS_BLOCK_SIZE	(1 << NGNFS_BLOCK_SHIFT)

struct ngnfs_block_ref {
	__le64 bnr;
	__le64 alloc_counter; /* XXX */
};

/*
 * The height is one greater than the level of the referenced block.
 * It's 0 for an empty tree.
 */
struct ngnfs_btree_root {
	struct ngnfs_block_ref ref;
	__u8 _pad[7];
	__u8 height;
};

/*
 * Keys are relatively large to allow precise deletion of index keys
 * which contain the generated 64bit key material as well as the logical
 * identity of the inode that generated the key.  The words in the key
 * value array are stored from most to least significant (k[0] is most
 * significant).
 */
struct ngnfs_btree_key {
	__le64 k[3];
};

/*
 * The first and last keys record the range of item keys that can be
 * found in the block.  It's dependent on (and redundant with) the keys
 * of separating parent items.  Having it in the block makes tracking
 * the range a bit easier to use and to update as we merge and split.
 */
struct ngnfs_btree_block {
	struct ngnfs_btree_key first;
	struct ngnfs_btree_key last;
	__le64 bnr;
	__le16 nr_items;
	__le16 tail_free;
	__le16 total_free;
	__u8 level;
	__u8 _pad[1];
	struct ngnfs_btree_item_header {
		__le16 off;
		__le16 val_size;
	} ihdrs[];
};

/*
 * The item's value payload is 64bit aligned and immediately follows the
 * item struct in the block.
 */
struct ngnfs_btree_item {
	struct ngnfs_btree_key key;
	__u8 val[];
};

/*
 * We want to avoid there only being a few items in a full block so we
 * chose a reasonably small fraction of the block size.  The array of
 * item headers is sized to fit the max number of items with no value
 * payload, while aligning the first item after the array.
 */
#define NGNFS_BTREE_MAX_VAL_SIZE	511
#define NGNFS_BTREE_ITEM_ALIGN		8
#define NGNFS_BTREE_MAX_ITEMS									\
	ALIGN_DOWN(((NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_btree_block)) /			\
		   (sizeof(struct ngnfs_btree_key) + sizeof(struct ngnfs_btree_item_header))),	\
		   NGNFS_BTREE_ITEM_ALIGN)
#define NGNFS_BTREE_MAX_FREE									\
	(NGNFS_BLOCK_SIZE - offsetof(struct ngnfs_btree_block, ihdrs[NGNFS_BTREE_MAX_ITEMS]))

/*
 * The inode generation number changes every time a specific inode is
 * freed and reallocated. The inode number and generation number
 * uniquely identify a file/dir over its lifetime. This lets users of
 * references to inodes find out if the inode has changed out from
 * underneath them. Generally, the inode number and generation number
 * should be referenced together, so make a struct to keep them
 * together.
 */

struct ngnfs_ino_gen {
	__le64 ino;	/* inode number, starts at 1 */
	__le64 gen;	/* inode generation, starts at 1 */
};

/*
 * Inodes are stored in inode blocks.  Inode blocks numbers are directly
 * calculated from the inode number.  The block itself is formatted as a
 * btree block and the inodes (and other inline inode data) are stored
 * as btree items in the block.
 */
struct ngnfs_inode {
	struct ngnfs_ino_gen ig;
	__le64 size;
	__le64 version;			/* changed on file content/metadata changes */
	struct ngnfs_ino_gen parent_ig;	/* only valid for directories */
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le32 flags;
	__le64 atime_nsec;
	__le64 ctime_nsec;
	__le64 mtime_nsec;
	__le64 crtime_nsec;
	struct ngnfs_btree_root dirents;
};

#define NGNFS_ROOT_INO 1
#define INIT_NGNFS_ROOT_IG { NGNFS_ROOT_INO, 1 }

/*
 * This is totally arbitrary.  It looks like it's 32bit in the stat ABI.
 * Most local file systems have around U16_MAX, but some have U32_MAX.
 */
#define NGNFS_LINK_MAX	S32_MAX

enum ngnfs_dentry_type {
	NGNFS_DT_FIFO = 0,
	NGNFS_DT_CHR,
	NGNFS_DT_DIR,
	NGNFS_DT_BLK,
	NGNFS_DT_REG,
	NGNFS_DT_LNK,
	NGNFS_DT_SOCK,
};

struct ngnfs_dirent {
	struct ngnfs_ino_gen ig; /* inode number and generation */
	__u8 pers_dtype; /* ngnfs persistent directory entry type */
	__u8 name_len; /* no null termination */
	__u8 name[6]; /* definition pads to alignment, stored can be smaller */
};

/* max dirent name length, without null term */
#define NGNFS_NAME_MAX	255

/* dirents must have at least 1 name byte */
#define NGNFS_DIRENT_MIN_VAL_SIZE offsetof(struct ngnfs_dirent, name[1])

/* just a random value */
#define NGNFS_DIRENT_HASH_SEED	0xce94cad8f038f79a

/*
 * The low bit of the dirent key value (and readdir pos) is manually
 * assigned to handle colliding name hash values.  We don't want the
 * unlikely event of a single hash collision to prevent creation.
 */
#define NGNFS_DIRENT_COLL_BIT	1ULL

/*
 * We clear the high bit to avoid signed long telldir/seekdir and
 * initially clear the collision bits.
 */
#define NGNFS_DIRENT_HASH_MASK	(U64_MAX ^ (1ULL << 63) ^ NGNFS_DIRENT_COLL_BIT)

/* reserved hash values for . and .. */
#define NGNFS_DIRENT_DOT_HASH	 	0ULL
#define NGNFS_DIRENT_DOT_DOT_HASH	1ULL
#define NGNFS_DIRENT_MIN_HASH		2ULL

#endif
