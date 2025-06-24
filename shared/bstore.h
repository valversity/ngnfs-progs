/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BSTORE_H
#define NGNFS_SHARED_BSTORE_H

#include "shared/format-dev.h"

void bstore_print_commit_block(struct ngnfs_dev_commit_block *cmt);
int bstore_check(struct ngnfs_dev_commit_block *cmt);

#endif
