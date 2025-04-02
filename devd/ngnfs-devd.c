/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Each devd process handles incoming network requests using a single
 * device.
 */

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/block.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/log.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "utask/block.h"
#include "utask/net.h"
#include "utask/utask.h"

#include "devd/proc.h"
#include "devd/cache-mode.h"

struct devd_options {
	char *dev_path;
	struct sockaddr_in listen_addr;
	char *trace_path;
};

static struct option_more devd_moreopts[] = {
	{ .longopt = { "device_path", required_argument, NULL, 'd' },
	  .arg = "path",
	  .desc = "path to block device",
	  .required = 1, },

	{ .longopt = { "listen_addr", required_argument, NULL, 'l' },
	  .arg = "addr:port",
	  .desc = "listening IPv4 address and port",
	  .required = 1, },

	{ .longopt = { "trace_file", required_argument, NULL, 't' },
	  .arg = "file_path",
	  .desc = "append debugging traces to this file",
	  .required = 1, },
};

static int parse_devd_opt(int c, char *str, void *arg)
{
	struct devd_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'd':
		ret = strdup_nerr(&opts->dev_path, str);
		break;
	case 'l':
		ret = parse_ipv4_addr_port(&opts->listen_addr, str);
		break;
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

#define BLOCK_QUEUE_DEPTH	32
#define NET_QUEUE_DEPTH		16 /* XXX no idea */

int main(int argc, char **argv)
{
	struct devd_options opts = { };
	int ret;

	ret = getopt_long_more(argc, argv, devd_moreopts, ARRAY_SIZE(devd_moreopts),
			       parse_devd_opt, &opts) ?:
	      trace_setup(opts.trace_path);
	if (ret < 0)
		goto out;

	ret = utask_init(BLOCK_QUEUE_DEPTH + NET_QUEUE_DEPTH) ?:
	      net_register_recv(proc_recv) ?:
	      net_listen(&opts.listen_addr, cache_release_all) ?:
	      block_init(opts.dev_path, BLOCK_QUEUE_DEPTH) ?:
	      utask_run();

	block_exit();
	utask_exit();
out:
	trace_destroy();
	return !!ret;
}
