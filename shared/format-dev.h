/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_DEV_H
#define NGNFS_SHARED_FORMAT_DEV_H

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/limits.h"
#include "shared/lk/types.h"

/*
 * This describes the structs that are only found in devices.  They're
 * used by devd to manage the blocks that are served over the network.
 */

enum {
	NGNFS_DEV_BLOCK_TYPE_UNINIT = 0,
	NGNFS_DEV_BLOCK_TYPE_COMMIT,
	NGNFS_DEV_BLOCK_TYPE_SUMMARY,
	NGNFS_DEV_BLOCK_TYPE_DETAILS,
	NGNFS_DEV_BLOCK_TYPE_STORED,
	NGNFS_DEV_BLOCK_TYPE__INVALID
};

#define NGNFS_UUID_SIZE	16
struct ngnfs_uuid {
	__u8 bytes[NGNFS_UUID_SIZE];
};

/*
 * Every internal metadata block starts with a header for verification.
 */
struct ngnfs_dev_block_header {
	__le64 crc;
	struct ngnfs_uuid dev_uuid;
	__u8 pad_[7];
	__u8 type;
};

/*
 * Devices are organized into large contiguous regions: a journal,
 * summaries, details, and the stored blocks themselves.
 */
struct ngnfs_dev_layout {
	__le64 commit_blocks;
	__le64 journal_blocks;
	__le64 summary_blocks;
	__le64 details_blocks;
	__le64 storage_blocks;
};

/*
 * Each commit block describes all the blocks involved in a coherent
 * atomic change.  The commit block and all its described blocks are
 * written concurrently.  They all need to be checked to verify that the
 * last commit succeeded.
 *
 * We spend some space in every commit block to store the device layout.
 * This saves us from having a separate long-lived format block.
 */
struct ngnfs_dev_commit_block {
	struct ngnfs_dev_block_header hdr;
	struct ngnfs_dev_layout layout;
	__le64 commit_ctr;
	__le64 oldest_commit_ctr;
	__le64 journal_head_ctr;
	__le64 journal_tail_ctr;
	__le16 nr_entries;
	__le16 nr_in_journal;
	__u8 pad_[4];
	struct ngnfs_dev_commit_entry {
		__le64 lba;
		__le64 journ_lba;
		__le64 crc;
		__u8 pad_[6];
		__u8 is_replay; /* XXX make a bit field */
		__u8 type;
	} entries[0];
};

#define NGNFS_DEV_COMMIT_MAX_ENTRIES					\
	((NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_dev_commit_block)) /	\
	 sizeof(struct ngnfs_dev_commit_entry))

/*
 * The network protocol operates-on per-block details in addition to its
 * contents.
 *
 * The lifetime_ctr is incremented every time the block is allocated or
 * freed.  An allocated block will always have an odd lifetime_ctr.
 *
 * The write_ctr is incremented every time the block is written and the
 * contents could have changed.  (The server doesn't pay the read IO
 * cost to check.)
 */
struct ngnfs_block_details {
	__le64 lifetime_ctr;
	__le64 write_ctr;
	__le32 crc;
	__u8 _pad[3];
	__u8 type;
};

struct ngnfs_dev_details_block {
	struct ngnfs_dev_block_header hdr;
	struct ngnfs_block_details details[0];
};

#define NGNFS_DEV_DETAILS_PER_BLOCK					\
	((NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_dev_details_block)) /	\
	 sizeof(struct ngnfs_block_details))

struct ngnfs_dev_summary_block {
	struct ngnfs_dev_block_header hdr;
	__le64 summaries[0];
};

#define NGNFS_DEV_SUMMARIES_PER_BLOCK					\
	((NGNFS_BLOCK_SIZE - sizeof(struct ngnfs_dev_summary_block)) /	\
	 sizeof(__le64))

#endif
