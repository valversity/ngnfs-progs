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
#include "shared/dtracef.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/log.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/thread.h"

#include "utask/block.h"
#include "utask/net.h"
#include "utask/sigcb.h"
#include "utask/utask.h"

#include "devd/bstore.h"
#include "devd/proc.h"
#include "devd/cache-mode.h"

struct devd_options {
	struct sockaddr_in mapd_server_addr;
	char *dev_path;
	struct sockaddr_in listen_addr;
	char *trace_path;
};

static struct option_more devd_moreopts[] = {
	{ .longopt = { "addr", required_argument, NULL, 'a' },
	  .arg = "addr:port",
	  .desc = "IPv4 address and port of mapd server to query",
	  .required = 1, },

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
	},
};

static int parse_devd_opt(int c, char *str, void *arg)
{
	struct devd_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'a':
		ret = parse_ipv4_addr_port(&opts->mapd_server_addr, str);
		break;
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

struct devd_main {
	struct devd_options opts;
	struct utask *tsk;
	int err;
};

static void signal_cb(siginfo_t *si, void *arg)
{
	struct devd_main *dm = arg;

	if (si->si_signo == SIGUSR1)
		utask_print_tasks();
	else
		utask_cancel(dm->tsk);
}

static void main_utask(void *data)
{
	struct devd_main *dm = data;
	struct signal_callback *sigcb = NULL;
	sigset_t set;
	int ret;

	/* arrange to shutdown if we get signals */
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGUSR1);
	ret = sigcb_register_callback(&set, signal_cb, dm, &sigcb);
	if (ret < 0)
		goto out;

	ret = block_init(dm->opts.dev_path, BLOCK_QUEUE_DEPTH) ?:
	      bstore_init() ?:
	      net_init() ?:
	      net_register_recv(proc_recv) ?:
	      net_listen(&dm->opts.listen_addr, cache_release_all) ?:
	      net_connect_mapd(&dm->opts.mapd_server_addr) ?:
	      utask_wait_event_task(utask_am_canceled());

	net_exit();
	bstore_exit();
	block_exit();
	sigcb_free(sigcb);
out:
	utask_shutdown();

	dm->err = ret;
}

int main(int argc, char **argv)
{
	struct devd_main dm = {{},};
	int ret;

	/* setup the most basic subsystems */
	ret = getopt_long_more(argc, argv, devd_moreopts, ARRAY_SIZE(devd_moreopts),
			       parse_devd_opt, &dm.opts) ?:
	      (dm.opts.trace_path ? dtracef_init(dm.opts.trace_path) : 0) ?:
	      utask_init(BLOCK_QUEUE_DEPTH + NET_QUEUE_DEPTH);
	if (ret < 0)
		goto out;

	/* switch over to the main utask for the rest of init */
	ret = utask_create(main_utask, &dm, &dm.tsk) ?:
	      utask_run();
out:
	utask_destroy(dm.tsk);
	utask_exit();
	dtracef_exit();

	return ret < 0 ? 1 : 0;
}
