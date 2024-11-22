/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "shared/lk/kernel.h"
#include "shared/lk/types.h"
#include "shared/lk/wait.h"

#include "shared/fs_info.h"
#include "shared/log.h"
#include "shared/map.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/shutdown.h"
#include "shared/thread.h"
#include "shared/trace.h"

#include "cli/cli.h"

struct map_options {
	struct sockaddr_in mapd_server_addr;
	char *trace_path;
};

static struct option_more map_moreopts[] = {
	{ .longopt = { "addr", required_argument, NULL, 'a' },
	  .arg = "addr:port",
	  .desc = "IPv4 address and port of mapd server to query",
	  .required = 1, },

	{ .longopt = { "trace_file", required_argument, NULL, 't' },
	  .arg = "file_path",
	  .desc = "append debugging traces to this file",
	  .required = 1, },
};

static int parse_map_opt(int c, char *str, void *arg)
{
	struct map_options *opts = arg;
	int ret = -EINVAL;

	switch(c) {
	case 'a':
		ret = parse_ipv4_addr_port(&opts->mapd_server_addr, str);
		break;
	case 't':
		ret = strdup_nerr(&opts->trace_path, str);
		break;
	}

	return ret;
}

/*
 * The triple-wrapped threading allows for cancellation and clean up
 * thusly:
 *
 * Thread 1: main(), waits for signals to initiate shutdown
 * Thread 2: map_thread(), does non-blocking setup, blocks, does shutdown
 * Thread 3: map_request_thread(), does blocking activities
 *
 * Thread 1 is a system-level monitor thread that keeps signals enabled
 * and listens for a signal to shutdown. Because ngnfs uses RCU, the
 * threads that actually call ngnfs routines have to have signals
 * blocked.
 *
 * Thread 2 is an ngnfs-level monitor thread that does non-blocking
 * setup, then spins off a thread to do blocking ops. It then waits for
 * either the child to complete, or the parent to tell it to shutdown.
 * When it wakes, it calls the various ngnfs shutdown functions, which
 * make all the threads shutdown and return gracefully.
 *
 * Thread 3 does actual IO. It will exit when it finishes, or when
 * thread 1 gets a signal, which causes thread 2 to call the shutdown
 * functions.
 */

struct map_request_thread_args {
	struct ngnfs_fs_info *nfi;
	struct sockaddr_in *mapd_server_addr;
	struct wait_queue_head *waitq;
	bool done;
	int ret;
};

static void map_request_thread(struct thread *thr, void *arg)
{
	struct map_request_thread_args *rargs = arg;
	struct ngnfs_fs_info *nfi = rargs->nfi;

	rargs->ret = ngnfs_map_get_maps(nfi);

	rargs->done = true;
	wake_up(rargs->waitq);
}

struct map_thread_args {
	int argc;
	char **argv;
	struct wait_queue_head waitq;
	int ret;
};

static void map_thread(struct thread *thr, void *arg)
{
	struct map_thread_args *margs = arg;
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	struct map_request_thread_args rargs = { };
	struct thread rthr;
	int ret;

	struct map_options opts = { };

	ret = getopt_long_more(margs->argc, margs->argv, map_moreopts, ARRAY_SIZE(map_moreopts),
			       parse_map_opt, &opts);
	if (ret < 0)
		goto out;

	thread_init(&rthr);
	rargs.nfi = &nfi;
	rargs.mapd_server_addr = &opts.mapd_server_addr;
	rargs.waitq = &margs->waitq;

	ret = trace_setup(opts.trace_path) ?:
	      ngnfs_map_setup(&nfi) ?:
	      ngnfs_msg_setup(&nfi, &ngnfs_mtr_socket_ops, NULL, NULL) ?:
	      ngnfs_map_client_setup(&nfi, &opts.mapd_server_addr) ?:
	      thread_start(&rthr, map_request_thread, &rargs);

	if (ret < 0)
		goto out;

	wait_event(&margs->waitq, rargs.done || thread_should_return(thr));
	ret = rargs.ret;

	if (rargs.done != true) {
		thread_stop_indicate(&rthr);
		thread_stop_wait(&rthr);
		ret = nfi.global_errno;
	}
out:
	margs->ret = ret;

	ngnfs_shutdown(&nfi, margs->ret);
	ngnfs_map_client_destroy(&nfi);
	ngnfs_msg_destroy(&nfi);
	ngnfs_map_destroy(&nfi);

	if (margs->ret < 0)
		log("error requesting map: "ENOF, ENOA(-margs->ret));
	else
		log("map received");
}

static int map_func(int argc, char **argv)
{
	struct map_thread_args margs = {
		.argc = argc,
		.argv = argv,
	};
	struct thread thr;
	int ret;

	thread_init(&thr);
	init_waitqueue_head(&margs.waitq);

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = thread_start(&thr, map_thread, &margs) ?:
	      thread_sigwait();

	thread_stop_indicate(&thr);
	wake_up(&margs.waitq);
	thread_stop_wait(&thr);

out:
	thread_finish_main();

	return ret ?: margs.ret;
}

static struct cli_command map_cmd = {
	.func = map_func,
	.name = "map",
	.desc = "request maps from mapd server",
};

CLI_REGISTER(map_cmd);
