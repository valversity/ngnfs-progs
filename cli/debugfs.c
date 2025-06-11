/* SPDX-License-Identifier: GPL-2.0 */

#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/lk/types.h"

#include "shared/dir.h"
#include "shared/format-block.h"
#include "shared/inode.h"
#include "shared/log.h"
#include "shared/mkfs.h"
#include "shared/mount.h"
#include "shared/nerr.h"
#include "shared/parse.h"
#include "shared/thread.h"

#include "cli/cli.h"

struct debugfs_context {
	struct ngnfs_inode_ino_gen cwd_ig;
	struct ngnfs_fs_info *nfi;
	bool quit;
	int ret;
};

#define LINE_SIZE (PATH_MAX * 5)
#define MAX_ARGC ((LINE_SIZE + 1) / 2)

/*
 * Print expected errors using printf; unexpected errors using log().
 */
static void print_err(char *cmd, int err)
{
	if ((err == -ENOENT) ||
	    (err == -EEXIST) ||
	    (err == -EISDIR) ||
	    (err == -ENOTDIR) ||
	    (err == -ENAMETOOLONG) ||
	    (err == -ENOBUFS) ||
	    (err == -EMLINK) ||
	    (err == -ENODATA) ||
	    (err == -ERANGE) ||
	    (err == -EINVAL))
		printf("%s error: "ENOF"\n", cmd, ENOA(-err));
	else
		log("%s unexpected error: "ENOF, cmd, ENOA(-err));
}

static void cmd_brename(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_dir_lookup_entry lent, sub_lent;
	char filename[NGNFS_NAME_MAX];
	char *subdir = "tmp";
	char *dir;
	u64 renames, max, i;
	int ret;

	if (argc != 3) {
		printf("usage: %s <directory name> <number renames>\n", argv[0]);
		return;
	}

	dir = argv[1];

	/* number of files is link max, minus . and .., minus the subdir */
	max = NGNFS_LINK_MAX - 3;
	ret = parse_ull(&renames, argv[2], 1, max);
	if (ret < 0) {
		printf("number of renames must be between 1 and %llu\n", max);
		return;
	}

	ret = ngnfs_dir_mkdir(ctx->nfi, &ctx->cwd_ig, 0755, dir, strlen(dir))			?:
	      ngnfs_dir_lookup(ctx->nfi, &ctx->cwd_ig, dir, strlen(dir), &lent)			?:
	      ngnfs_dir_mkdir(ctx->nfi, &lent.ig, 0755, subdir, strlen(subdir))			?:
	      ngnfs_dir_lookup(ctx->nfi, &lent.ig, subdir, strlen(subdir), &sub_lent);

	if (ret < 0) {
		print_err("brename: mkdir", ret);
		return;
	}

	for (i = 0; i < renames; i++) {
		snprintf(filename, sizeof(filename), "%llu", i);

		ret = ngnfs_dir_create(ctx->nfi, &sub_lent.ig, 0644, filename, strlen(filename));
		if (ret < 0) {
			print_err("brename: create", ret);
			return;
		}

		ret = ngnfs_dir_rename(ctx->nfi, &sub_lent.ig, filename, strlen(filename), &lent.ig,
				       filename, strlen(filename));
		if (ret < 0) {
			print_err("brename: rename", ret);
			return;
		}
	}
}

static void cmd_cd(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_dir_lookup_entry lent;
	char *name;
	int ret;

	if (argc != 2) {
		printf("usage: cd <directory name>\n");
		return;
	}

	name = argv[1];

	ret = ngnfs_dir_lookup(ctx->nfi, &ctx->cwd_ig, name, strlen(name), &lent);
	if (ret < 0) {
		print_err("cd", ret);
		return;
	}

	if (lent.dtype != DT_DIR) {
		printf("cd: not a directory: %s", name);
		return;
	}

	ctx->cwd_ig = lent.ig;
}

static void cmd_create(struct debugfs_context *ctx, int argc, char **argv)
{
	int ret;

	if (argc != 2) {
		printf("must have one, and only one, file name to create\n");
		return;
	}

	ret = ngnfs_dir_create(ctx->nfi, &ctx->cwd_ig, 0644, argv[1], strlen(argv[1]));
	if (ret < 0)
		print_err("create", ret);
}

static void cmd_lookup(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_dir_lookup_entry lent;
	char *name;
	int name_len;
	int ret;

	if (argc != 2) {
		printf("usage: lookup <pathname>\n");
		return;
	}

	name = argv[1];
	name_len = strlen(name);

	ret = ngnfs_dir_lookup(ctx->nfi, &ctx->cwd_ig, name, name_len, &lent);
	if (ret < 0) {
		print_err("lookup", ret);
		return;
	}

	printf("name: %.*s\n"
	       "name_len: %d\n"
	       "dtype: %o\n"
	       "ino: %llu\n"
	       "gen: %llu\n",
	       name_len,
	       name,
	       name_len,
	       lent.dtype,
	       lent.ig.ino,
	       lent.ig.gen);
}

static void cmd_mkdir(struct debugfs_context *ctx, int argc, char **argv)
{
	int ret;

	if (argc != 2) {
		printf("must have one, and only one, directory name to create\n");
		return;
	}

	ret = ngnfs_dir_mkdir(ctx->nfi, &ctx->cwd_ig, 0755, argv[1], strlen(argv[1]));
	if (ret < 0)
		print_err("mkdir", ret);
}

static void cmd_mkfs(struct debugfs_context *ctx, int argc, char **argv)
{
	int ret;

	ret = ngnfs_mkfs(ctx->nfi);
	if (ret < 0) {
		print_err("mkfs", ret);
		return;
	}

	ret = ngnfs_block_sync(ctx->nfi);
	if (ret < 0)
		print_err("mkfs: final sync", ret);
}

static void cmd_quit(struct debugfs_context *ctx, int argc, char **argv)
{
	ctx->quit = true;
	return;
}

static void cmd_readdir(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_readdir_entry *buf;
	struct ngnfs_readdir_entry *ent;
	const int size = NGNFS_READDIR_MIN_BUF_SIZE * 10;
	u64 total = 0;
	u64 pos;
	int ret;
	int i;

	if (argc > 2) {
		printf("usage: readdir [position]\n");
		return;
	}

	pos = 0;
	if (argc == 2) {
		ret = strtoull_nerr(&pos, argv[1], NULL, 0);
		if (ret < 0) {
			printf("invalid position: %s\n", argv[1]);
			return;
		}
	}

	buf = malloc(size);
	if (!buf) {
		printf("malloc error");
		return;
	}

	while (true) {
		ret = ngnfs_dir_readdir(ctx->nfi, &ctx->cwd_ig, pos, buf, size);
		if (ret <= 0) {
			if (ret < 0)
				print_err("readdir", ret);
			break;
		}

		ent = buf;
		for (i = 0; i < ret; i++) {
			printf("%020llu %10llu %5llu %05o %.*s\n", ent->pos, ent->ig.ino,
			       ent->ig.gen, ent->dtype, ent->name_len, ent->name);
			pos = ent->pos;
			ent = (void *)ent + ent->next_offset;
		}

		total += ret;
		if (pos++ == U64_MAX)
			break;
	}
	printf("total %llu\n", total);

	free(buf);
}

/*
 * XXX Basic rename hack for now, allows moving a file from the current
 * directory to its parent dir.
 */
static void cmd_rename(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_dir_lookup_entry parent;
	char *name;
	size_t name_len;
	int ret;

	if (argc != 2) {
		printf("usage: rename <filename>\n");
		printf("will move <filename> to ..\n");
		return;
	}

	name = argv[1];
	name_len = strlen(name);

	ret = ngnfs_dir_lookup(ctx->nfi, &ctx->cwd_ig, "..", strlen(".."), &parent)		?:
	      ngnfs_dir_rename(ctx->nfi, &ctx->cwd_ig, name, name_len, &parent.ig, name, name_len);

	if (ret < 0)
		print_err("rename", ret);
}

static void cmd_rmdir(struct debugfs_context *ctx, int argc, char **argv)
{
	char *name;
	int ret;

	if (argc != 2) {
		printf("usage: rmdir <pathname>\n");
		return;
	}

	name = argv[1];

	ret = ngnfs_dir_rmdir(ctx->nfi, &ctx->cwd_ig, name, strlen(name));
	if (ret < 0)
		print_err("rmdir", ret);
}

static void cmd_stat(struct debugfs_context *ctx, int argc, char **argv)
{
	struct ngnfs_inode_ino_gen ig = ctx->cwd_ig;
	struct ngnfs_dir_lookup_entry lent;
	struct ngnfs_inode ninode;
	char *name;
	int ret;

	if (argc > 2) {
		printf("usage: stat [filename]\n");
		return;
	}

	if (argc == 2) {
		name = argv[1];
		ret = ngnfs_dir_lookup(ctx->nfi, &ctx->cwd_ig, name, strlen(name), &lent);
		if (ret < 0) {
			print_err("stat", ret);
			return;
		}
		ig = lent.ig;
	}

	ret = ngnfs_inode_read_copy(ctx->nfi, &ig, &ninode, sizeof(ninode));

	if (ret < 0) {
		print_err("stat", ret);
	} else if (ret < sizeof(ninode)) {
		log("returned inode buffer size %d too small, wanted at least %zu",
		    ret, sizeof(ninode));
	} else {
		printf("ino: %llu\n"
		       "gen: %llu\n"
		       "size: %llu\n"
		       "nlink: %u\n"
		       "mode: %o\n"
		       "parent ino: %llu\n"
		       "parent gen: %llu\n"
		       "atime: %llu\n"
		       "ctime: %llu\n"
		       "mtime: %llu\n"
		       "crtime: %llu\n",
		       le64_to_cpu(ninode.ig.ino),
		       le64_to_cpu(ninode.ig.gen),
		       le64_to_cpu(ninode.size),
		       le32_to_cpu(ninode.nlink),
		       le32_to_cpu(ninode.mode),
		       le64_to_cpu(ninode.parent_ig.ino),
		       le64_to_cpu(ninode.parent_ig.gen),
		       le64_to_cpu(ninode.atime_nsec),
		       le64_to_cpu(ninode.ctime_nsec),
		       le64_to_cpu(ninode.mtime_nsec),
		       le64_to_cpu(ninode.crtime_nsec));
	}
}

static void cmd_sync(struct debugfs_context *ctx, int argc, char **argv)
{
	int ret;

	ret = ngnfs_block_sync(ctx->nfi);
	if (ret < 0)
		print_err("sync", ret);
}

static void cmd_unlink(struct debugfs_context *ctx, int argc, char **argv)
{
	char *name;
	int ret;

	if (argc != 2) {
		printf("usage: unlink <pathname>\n");
		return;
	}

	name = argv[1];

	ret = ngnfs_dir_unlink(ctx->nfi, &ctx->cwd_ig, name, strlen(name));
	if (ret < 0)
		print_err("unlink", ret);
}

static struct command {
	char *name;
	void (*func)(struct debugfs_context *ctx, int argc, char **argv);
} commands[] = {
	{ "brename", cmd_brename, },
	{ "cd", cmd_cd, },
	{ "create", cmd_create, },
	{ "lookup", cmd_lookup, },
	{ "mkdir", cmd_mkdir, },
	{ "mkfs", cmd_mkfs, },
	{ "quit", cmd_quit, },
	{ "readdir", cmd_readdir, },
	{ "rename", cmd_rename, },
	{ "rmdir", cmd_rmdir, },
	{ "stat", cmd_stat, },
	{ "sync", cmd_sync, },
	{ "unlink", cmd_unlink, },
};

static int compar_cmd_names(const void *A, const void *B)
{
	const struct command *a = A;
	const struct command *b = B;

	return strcmp(a->name, b->name);
}

static int compar_key_cmd_name(const void *key, const void *ele)
{
	const char *name = key;
	const struct command *cmd = ele;

	return strcmp(name, cmd->name);
}

static void parse_command(struct debugfs_context *ctx, char *buf, char **argv)
{
	struct command *cmd;
	char *delim = "\t \n\r";
	char *saveptr;
	char *str;
	int argc;

	for (argc = 0, str = buf, saveptr = NULL; argc < MAX_ARGC; argc++, str = NULL) {
		argv[argc] = strtok_r(str, delim, &saveptr);
		if (argv[argc] == NULL)
			break;
	}

	if (argc == 0) {
		printf("no command");
		return;
	}

	cmd = bsearch(argv[0], commands, ARRAY_SIZE(commands), sizeof(commands[0]),
		      compar_key_cmd_name);
	if (!cmd) {
		printf("unknown command: '%s'\n", argv[0]);
		return;
	}

	cmd->func(ctx, argc, argv);
}

static void debugfs_thread(struct thread *thr, void *arg)
{
	struct debugfs_context *ctx = arg;
	char **line_argv = NULL;
	char *line = NULL;
	int ret;
	bool is_tty = isatty(STDIN_FILENO);

	line = malloc(LINE_SIZE);
	line_argv = calloc(MAX_ARGC, sizeof(line_argv[0]));
	if (!line || !line_argv) {
		ret = -ENOMEM;
		goto out;
	}

	/* make sure command names are sorted for bsearch */
	qsort(commands, ARRAY_SIZE(commands), sizeof(commands[0]), compar_cmd_names);

	for (;;) {
		fprintf(stdout, "<%llu> $ ", ctx->cwd_ig.ino);
		fflush(stdout);
		if (!fgets(line, LINE_SIZE, stdin))
			break;

		/*
		 * Copy the entire line (with \n at the end) back to the output if
		 * we're reading from a pipe. This makes the output readable like a
		 * full log of what commands were performed.
		 */
		if (!is_tty)
			fprintf(stdout, "%s", line);

		parse_command(ctx, line, line_argv);

		if (ctx->quit)
			break;
	}

	ret = ngnfs_block_sync(ctx->nfi);
	if (ret < 0)
		print_err("final sync", ret);
out:
	free(line);
	free(line_argv);
	ctx->ret = ret;
	kill(getpid(), SIGUSR1);
}

/*
 * We have the debugfs command run in a thread so that it can call ngnfs
 * client operations (pfs, block, txn) directly.  That dictates its
 * signal handling behaviour and makes it uninterruptible.  We park this
 * initial cli command function as a monitoring thread that can stop the
 * debugfs thread when it catches signals.
 */
static int debugfs_func(int argc, char **argv)
{
	struct ngnfs_fs_info nfi = INIT_NGNFS_FS_INFO;
	struct debugfs_context ctx = {
		.nfi = &nfi,
		.cwd_ig = INIT_NGNFS_ROOT_IG,
		.quit = false,
	};
	struct thread thr;
	int ret;

	thread_init(&thr);

	ret = ngnfs_mount(&nfi, argc, argv);
	if (ret < 0)
		goto out;

	ret = thread_start(&thr, debugfs_thread, &ctx) ?:
	      thread_sigwait();

	thread_stop_wait(&thr);
	ngnfs_unmount(&nfi);
	thread_finish_main();
out:
	return ret ?: ctx.ret;
}

static struct cli_command debugfs_cmd = {
	.func = debugfs_func,
	.name = "debugfs",
	.desc = "debugfs desc",
};

CLI_REGISTER(debugfs_cmd);
