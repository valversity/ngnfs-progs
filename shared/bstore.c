/* SPDX-License-Identifier: GPL-2.0 */

#include <errno.h>
#include <stdio.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/math.h"
#include "shared/lk/overflow.h"
#include "shared/lk/types.h"

#include "shared/bstore.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"

#include "utask/block.h"

void bstore_print_commit_block(struct ngnfs_dev_commit_block *cmt)
{
	printf("commit blocks  = %12llu\n"
	       "journal blocks = %12llu\n"
	       "summary blocks = %12llu\n"
	       "details blocks = %12llu\n"
	       "storage blocks = %12llu\n",
	       le64_to_cpu(cmt->layout.commit_blocks),
	       le64_to_cpu(cmt->layout.journal_blocks),
	       le64_to_cpu(cmt->layout.summary_blocks),
	       le64_to_cpu(cmt->layout.details_blocks),
	       le64_to_cpu(cmt->layout.storage_blocks));

}

int bstore_check(struct ngnfs_dev_commit_block *cmt)
{
	u64 commit_blocks;
	u64 journal_blocks;
	u64 summary_blocks;
	u64 details_blocks;
	u64 storage_blocks;
	int ret;

	commit_blocks = le64_to_cpu(cmt->layout.commit_blocks);
	journal_blocks = le64_to_cpu(cmt->layout.journal_blocks);
	summary_blocks = le64_to_cpu(cmt->layout.summary_blocks);
	details_blocks = le64_to_cpu(cmt->layout.details_blocks);
	storage_blocks = le64_to_cpu(cmt->layout.storage_blocks);

	/* arbitrary tiny mins for commits/journal */
	if (commit_blocks < 256									||
	    journal_blocks < 256								||
	    summary_blocks < DIV_ROUND_UP(details_blocks, NGNFS_DEV_SUMMARIES_PER_BLOCK)	||
	    details_blocks < DIV_ROUND_UP(storage_blocks, NGNFS_DEV_DETAILS_PER_BLOCK)) {
		printf("%s: not enough blocks of a specific type, need larger device\n", __func__);
		bstore_print_commit_block(cmt);
		ret = -EINVAL;
		goto out;
	}

	/* ulong max for the size of the stable_ht */
	if (journal_blocks >= ULONG_MAX) {
		printf("%s: # journal blocks too big: %llu > %lu\n",
		       __func__, journal_blocks, ULONG_MAX);
		bstore_print_commit_block(cmt);
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	return ret;
}
