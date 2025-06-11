/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_DIR_H
#define NGNFS_SHARED_DIR_H

#include "shared/fs_info.h"
#include "shared/inode.h"

int ngnfs_dir_create(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir, umode_t mode,
		     char *name, size_t name_len);
int ngnfs_dir_mkdir(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir, umode_t mode,
		    char *name, size_t name_len);
int ngnfs_dir_unlink(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir, char *name,
		     size_t name_len);
int ngnfs_dir_rmdir(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir, char *name,
		    size_t name_len);
int ngnfs_dir_rename(struct ngnfs_fs_info *nfi,
		     struct ngnfs_inode_ino_gen *src_dir_ig, char *src_name, size_t src_name_len,
		     struct ngnfs_inode_ino_gen *dst_dir_ig, char *dst_name, size_t dst_name_len);

/*
 * Readdir fills the buffer with entries.  The start of the buffer must
 * be at least as aligned as the entry.  Entries are padded for
 * alignment.  Use next_off to iterate through them instead of trying to
 * skip name length bytes past the end of the struct.
 */
struct ngnfs_readdir_entry {
	u64 pos;
	struct ngnfs_inode_ino_gen ig;
	u16 next_offset;		/* bytes to add to entry to get next entry */
	u8 dtype;
	u8 name_len;			/* does not include terminating null, like strlen */
	u8 name[];			/* is null terminated, name_len doesn't include null byte */
};

/*
 * Minimum readdir entry buf size needed to store a single entry with a
 * maximum name size.  The buffer must be this size or an error is
 * returned.
 */
#define NGNFS_READDIR_MIN_BUF_SIZE \
	offsetof(struct ngnfs_readdir_entry , name[NGNFS_NAME_MAX + 1])

/*
 * Returns the number of entries filled into the buffer, not the number
 * of bytes.
 */
int ngnfs_dir_readdir(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir_ig, u64 pos,
		      struct ngnfs_readdir_entry *buf, size_t size);

/*
 * Lookup returns only the inode number, generation, and the POSIX ABI
 * file type.
 */
struct ngnfs_dir_lookup_entry {
	struct ngnfs_inode_ino_gen ig;
	u8 dtype;
};

int ngnfs_dir_lookup(struct ngnfs_fs_info *nfi, struct ngnfs_inode_ino_gen *dir_ig, char *name,
		     size_t name_len, struct ngnfs_dir_lookup_entry *lent);

#endif
