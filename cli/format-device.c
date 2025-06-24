/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* canonicalize_file_name() */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/crc64.h"
#include "shared/lk/math.h"

#include "shared/bstore.h"
#include "shared/devfd.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"
#include "shared/log.h"

#include "cli/cli.h"

static int format_device_func(int argc, char **argv)
{
	struct ngnfs_dev_commit_block *cmt = NULL;
	char uuid_str[37];
	uuid_t uuid;
	u64 size;
	u64 total;
	u64 commit;
	u64 journal;
	u64 summary;
	u64 details;
	u64 store;
	u64 crc;
	u64 d;
	u64 s;
	int fd = -1;
	int off;
	int ret;

	/*
	 * XXX how are we doing option parsing in commands?
	 */
	if (argc != 2) {
		printf("incorrect argc %d\n", argc);
		ret = -EINVAL;
		goto out;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		ret = -errno;
		printf("error opening '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	ret = devfd_get_size(fd, &size);
	if (ret < 0) {
		printf("getting size: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	total = size / NGNFS_BLOCK_SIZE;
	journal = total >> 10;
	commit = journal >> 2;
	journal -= commit;
	summary = 1;
	details = 1;
	store = total;
	do {
		d = details;
		s = summary;
		details = DIV_ROUND_UP(store, NGNFS_DEV_DETAILS_PER_BLOCK);
		summary = DIV_ROUND_UP(details, NGNFS_DEV_SUMMARIES_PER_BLOCK);
		store = total - commit - journal - summary - details;
	} while (d != details || s != summary);

	ret = devfd_write_zeros(fd, 0, (total - store) * NGNFS_BLOCK_SIZE);
	if (ret < 0) {
		printf("zeroing metadata: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	cmt = calloc(1, NGNFS_BLOCK_SIZE);
	cmt->hdr.type = NGNFS_DEV_BLOCK_TYPE_COMMIT;
	cmt->layout.commit_blocks = cpu_to_le64(commit);
	cmt->layout.journal_blocks = cpu_to_le64(journal);
	cmt->layout.summary_blocks = cpu_to_le64(summary);
	cmt->layout.details_blocks = cpu_to_le64(details);
	cmt->layout.storage_blocks = cpu_to_le64(store);

	ret = bstore_check(cmt);
	if (ret < 0)
		goto out;

	uuid_generate_random(uuid);
	uuid_unparse(uuid, uuid_str);
	BUILD_BUG_ON(NGNFS_UUID_SIZE != sizeof(uuid_t));
	memcpy(&cmt->hdr.dev_uuid, &uuid, NGNFS_UUID_SIZE);

	off = sizeof_field(struct ngnfs_dev_block_header, crc);
	crc = crc64_nvme(0, (void *)cmt + off, NGNFS_BLOCK_SIZE - off);
	cmt->hdr.crc = cpu_to_le64(crc);

	ret = pwrite(fd, cmt, NGNFS_BLOCK_SIZE, 0);
	if (ret != NGNFS_BLOCK_SIZE) {
		if (ret >= 0)
			ret = -EIO;
		else
			ret = -errno;
		printf("write error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	ret = fdatasync(fd);
	if (ret != 0) {
		ret = -errno;
		printf("fdatasync error: '%s': "ENOF"\n", argv[1], ENOA(-ret));
		goto out;
	}

	printf("Formatted ngnfs device:\n"
	       "  uuid:                   %s\n"
	       "  4KiB blocks:            %llu\n",
		uuid_str,
		total);

	ret = 0;
out:
	free(cmt);
	if (fd >= 0)
		close(fd);

	return ret;
}

static struct cli_command format_device_cmd = {
	.func = format_device_func,
	.name = "format-device",
	.desc = "Write to device for identification by ngnfs, (destructive!)"
};

CLI_REGISTER(format_device_cmd);
