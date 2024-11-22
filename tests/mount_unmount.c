

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

#include "shared/fs_info.h"
#include "shared/log.h"
#include "shared/mount.h"
#include "shared/nerr.h"
#include "shared/options.h"
#include "shared/parse.h"
#include "shared/shutdown.h"
#include "shared/thread.h"

struct map_thread_args {
	int argc;
	char **argv;
	int ret;
};

static void mount_unmount_thread(struct thread *thr, void *arg)
{
	struct map_thread_args *margs = arg;
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	int ret;

	ret = ngnfs_mount(&nfi, margs->argc, margs->argv);
	if (ret < 0)
		goto out;

	ngnfs_unmount(&nfi);
out:
	margs->ret = ret ?: nfi.global_errno;
	ngnfs_shutdown(&nfi, margs->ret);
}

int main(int argc, char **argv)
{
	struct map_thread_args margs = {
		.argc = argc,
		.argv = argv,
	};
	struct thread thr;
	int ret;

	thread_init(&thr);

	ret = thread_prepare_main();
	if (ret < 0)
		goto out;

	ret = thread_start(&thr, mount_unmount_thread, &margs) ?:
	      thread_sigwait();

	thread_stop_indicate(&thr);
	thread_stop_wait(&thr);

out:
	thread_finish_main();

	return ret ?: margs.ret;
}

