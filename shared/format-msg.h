/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_MSG_H
#define NGNFS_SHARED_FORMAT_MSG_H

#include <linux/types.h>

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/types.h"

enum {
	NGNFS_MSG_BLOCK_READ = 0,
	NGNFS_MSG_BLOCK_READ_RESULT,
	NGNFS_MSG_BLOCK_WRITE,
	NGNFS_MSG_BLOCK_WRITE_RESULT,
	NGNFS_MSG_BLOCK_MODE_SET,
	NGNFS_MSG_BLOCK_MODE_ACK,
	NGNFS_MSG_GET_MAPS,
	NGNFS_MSG_GET_MAPS_RESULT,
	NGNFS_MSG__NR,
};

enum {
	NGNFS_MSG_ERR_OK = 0,
	NGNFS_MSG_ERR_UNKNOWN,
	NGNFS_MSG_ERR_EIO,
	NGNFS_MSG_ERR_ENOMEM,
	NGNFS_MSG_ERR__INVALID,
};

/*
 * _NULL is 0 so we can have natural if (mode) tests.  _NONE is the mode
 * that grants no access and causes the client and server to free
 * tracking of the cached block.
 *
 * The modes are also numerically sorted from least to most restrictive.
 * NONE is compat with everything, read isn't compatible with write, and
 * write is compatible with nothing.
 */
enum {
	NGNFS_CACHE_MODE_NULL = 0,
	NGNFS_CACHE_MODE_NONE,
	NGNFS_CACHE_MODE_READ,
	NGNFS_CACHE_MODE_WRITE,
	NGNFS_CACHE_MODE__INVALID,
};

struct ngnfs_msg_header {
	__le32 crc;
	__le16 data_size;
	__u8 ctl_size;
	__u8 type;
};

#define NGNFS_MSG_MAX_CTL_SIZE	255
#define NGNFS_MSG_MAX_DATA_SIZE 4096

struct ngnfs_msg_block_read {
	__le64 bnr;
	__le64 flags;
	__u8 mode;
	__u8 _pad[7];
};

struct ngnfs_msg_block_read_result {
	__le64 bnr;
	__u8 mode;
	__u8 err;
	__u8 _pad[6];
};

#define NGNFS_MSG_BLOCK_READ_FLAG_NO_DATA	(1 << 0)

struct ngnfs_msg_block_write {
	__le64 bnr;
	__u8 mode;
	__u8 _pad[7];
};

struct ngnfs_msg_block_write_result {
	__le64 bnr;
	__u8 err;
	__u8 _pad[7];
};

struct ngnfs_msg_cache_mode {
	__le64 bnr;
	__u8 mode;
	__u8 _pad[7];
};

struct ngnfs_devd_map {
	__le64 version;
	__le64 nr_addrs;
	struct ngnfs_ipv4_addr {
		__le64 device_uuid; /* XXX future :) */
		__le32 addr;
		__le16 port;
		__le16 _pad;
	} addrs[];
};

/* Eventually this will have more than one map. */
struct ngnfs_maps {
	struct ngnfs_devd_map devd_map;
};

/*
 * TODO: currently a message can't have both ctl_size and buf_size of 0.
 * Either this will expand to have something in it or we will allow
 * that.
 *
 * XXX still true?
 */
struct ngnfs_msg_get_maps {
	__u8 _pad[8];
};

struct ngnfs_msg_get_maps_result {
	__u8 err;
	__u8 _pad[7];
	struct ngnfs_devd_map devd_map;
};

#endif
