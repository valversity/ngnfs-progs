/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE /* O_DIRECT */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "shared/lk/err.h"
#include "shared/lk/list.h"
#include "shared/lk/rbtree.h"

#include "shared/clist.h"
#include "shared/devfd.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/hash_table.h"

#include "utask/block.h"
#include "utask/utask.h"

/*
 * This provides a block cache on top of local storage for tasks in our
 * utask runtime with IO managed by io_uring.
 *
 * The interface provided is relatively lowlevel.  Callers get
 * references to blocks and can block waiting for IO, but there's no
 * read/write exclusion.  That's entirely the responsibility of the
 * caller.
 *
 * Callers can create dirty blocks, which we record on a list, and then
 * provide a call for writing all dirty blocks.  This mechanism is built
 * for the particular needs of the only devd user.  It's making block
 * changes persistent before sending responses to requests.  Dirty
 * blocks are short lived.
 */

static struct block_cache_instance {
	struct hash_table *cache_ht;
	struct list_head submit_list;
	struct counted_list_head hot_fifo;
	struct counted_list_head cold_fifo;
	struct list_head dirty_list;
	struct utask *submit_tsk;
	struct utask *exit_tsk;
	struct utask_wait_queue write_wq;
	u64 total_blocks;
	unsigned long nr_hashed;
	unsigned long nr_dirty;
	unsigned long nr_dirty_submitted;
	unsigned long nr_in_flight;
	unsigned long queue_depth;
	int write_err;
	int dev_fd;

} global_block_cache_inst = {
	.submit_list = LIST_HEAD_INIT(global_block_cache_inst.submit_list),
	.hot_fifo.head = LIST_HEAD_INIT(global_block_cache_inst.hot_fifo.head),
	.cold_fifo.head = LIST_HEAD_INIT(global_block_cache_inst.cold_fifo.head),
	.dirty_list = LIST_HEAD_INIT(global_block_cache_inst.dirty_list),
	.write_wq = INIT_UTASK_WAIT_QUEUE(global_block_cache_inst.write_wq),
	.dev_fd = -1,
};

struct cached_block {
	struct list_head submit_head;
	struct list_head fifo_head;
	struct list_head dirty_head;
	struct utask_wait_queue wq;
	struct utask_cqe_callback cb;
	struct page *data_page;
	u64 bnr;
	long refcount;
	int error;
	/* awkward union magic only for tracing flags */
	union {
		unsigned int all_flags_;
		struct {
			unsigned int accessed:1,
				     hashed:1,
				     queued:1,
				     uptodate:1,
				     verified:1,
				     dirty:1;
		};
	};
};

#define CBLK_FMT	"cblk %p bnr %llu refcount %ld flags 0x%02x"
#define CBLK_ARGS(CB_)	(CB_), (CB_)->bnr, (CB_)->refcount, (CB_)->all_flags_

/*
 * A wrapper around the arguments for tracing blocks.  It can also just
 * do the fmt/args without a block, like in return paths with errors.
 */
#define DTRACEF_CBLK(str, cblk, fmt, args...)					\
do {										\
	typeof(cblk) cblk_ = (cblk);						\
										\
	if (IS_ERR_OR_NULL(cblk_))						\
		dtracef(str, "bad_cblk %p " fmt, cblk_, ##args);		\
	else									\
		dtracef(str, CBLK_FMT " " fmt, CBLK_ARGS(cblk_), ##args);	\
} while (0)

static struct cached_block *alloc_cblk(struct block_cache_instance *inst)
{
	struct cached_block *cblk;

	cblk = calloc(1, sizeof(struct cached_block));
	if (!cblk || !(cblk->data_page = alloc_page(GFP_NOFS))) {
		free(cblk);
		return NULL;
	}

	INIT_LIST_HEAD(&cblk->submit_head);
	INIT_LIST_HEAD(&cblk->fifo_head);
	INIT_LIST_HEAD(&cblk->dirty_head);
	utask_init_wait_queue(&cblk->wq);
	cblk->refcount = 1;

	DTRACEF_CBLK("block_alloc", cblk, "");

	return cblk;
}

static void free_cblk(struct cached_block *cblk)
{
	DTRACEF_CBLK("block_free", cblk, "");

	WARN_ON_ONCE(cblk->refcount != 0);
	WARN_ON_ONCE(cblk->hashed != 0);
	WARN_ON_ONCE(!list_empty(&cblk->submit_head));
	WARN_ON_ONCE(!list_empty(&cblk->fifo_head));
	WARN_ON_ONCE(!list_empty(&cblk->dirty_head));
	WARN_ON_ONCE(utask_waitqueue_active(&cblk->wq));

	__free_page(cblk->data_page);
	free(cblk);
}

static void get_cblk(struct cached_block *cblk)
{
	BUG_ON(cblk->refcount < 1);
	BUG_ON(cblk->refcount == LONG_MAX);
	cblk->refcount++;
}

static void put_cblk(struct cached_block *cblk)
{
	BUG_ON(cblk->refcount < 1);

	if (--cblk->refcount == 0)
		free_cblk(cblk);
}

static struct cached_block *del_first_pool_block(struct list_head *pool)
{
	struct cached_block *cblk;

	cblk = list_first_entry_or_null(pool, struct cached_block, fifo_head);
	if (cblk)
		list_del_init(&cblk->fifo_head);

	return cblk;
}

static void unhash_cblk(struct block_cache_instance *inst, struct cached_block *cblk)
{
	if (cblk->hashed) {
		DTRACEF_CBLK("block_unhash", cblk, "");
		htable_delete(inst->cache_ht, cblk->bnr);
		cblk->hashed = 0;
		inst->nr_hashed--;
		put_cblk(cblk);
	}
}

/*
 * XXX Until configurable, we chose an arbitrary limit on the cache size
 * that seems not tiny but also won't overwhelm guests with small
 * amounts of memory and a handful of processes.
 */
#define MAX_CACHED_BLOCKS	(256 * 1024 * 1024 / NGNFS_BLOCK_SIZE)
#define HOT_FIFO_PCT		10
#define COLD_FIFO_PCT		(100 - HOT_FIFO_PCT)

static struct cached_block *clist_del_first_past_pct(struct block_cache_instance *inst,
						     struct counted_list_head *clist,
						     unsigned long pct)
{
	struct cached_block *cblk;

	if ((clist->count * 100 / MAX_CACHED_BLOCKS) < pct)
		return NULL;

	cblk = list_first_entry_or_null(&clist->head, struct cached_block, fifo_head);
	if (cblk)
		clist_del_init(&cblk->fifo_head, clist);
	return cblk;
}

/*
 * Try and shrink the cache.  Blocks have a single accessed bit that's
 * set as they're looked up in the hash table.  New blocks are added to
 * a small hot fifo.  If they're accessed as they're removed from either
 * fifo, they're added to the cold fifo.  If they're not accessed as
 * they leave either fifo then they're freed.
 *
 * See: Yang, Juncheng; Qiu, Ziyue; Zhang, Yazhuo; Yue, Yao; Rashmi, K.
 * V. (22 June 2023). "FIFO can be Better than LRU: The Power of Lazy
 * Promotion and Quick Demotion"
 *
 * This is what they'd call qd-lp-clock1, I think.  We could add more
 * accessed bits.
 */
static void try_shrink(struct block_cache_instance *inst)
{
	struct cached_block *cblk;

	while ((cblk = clist_del_first_past_pct(inst, &inst->hot_fifo, HOT_FIFO_PCT) ?:
		       clist_del_first_past_pct(inst, &inst->cold_fifo, COLD_FIFO_PCT))) {

		DTRACEF_CBLK("block_shrink", cblk, "");

		if (cblk->accessed) {
			clist_add_tail(&cblk->fifo_head, &inst->cold_fifo);
			cblk->accessed = 0;
		} else {
			put_cblk(cblk);
			unhash_cblk(inst, cblk);
		}
	}
}

static struct cached_block *lookup_cblk(struct block_cache_instance *inst, u64 bnr)
{
	struct cached_block *cblk;

	if (WARN_ON_ONCE(bnr >= inst->total_blocks))
		return NULL;

	cblk = (struct cached_block *)htable_lookup(inst->cache_ht, bnr);
	if (cblk) {
		cblk->accessed = 1;
		get_cblk(cblk);
	}

	return cblk;
}

static struct cached_block *lookup_or_alloc_cblk(struct block_cache_instance *inst,
						 struct list_head *pool, u64 bnr)
{
	struct cached_block *cblk;

	cblk = lookup_cblk(inst, bnr);
	if (!cblk) {
		if (pool)
			cblk = del_first_pool_block(pool);
		else
			cblk = alloc_cblk(inst);
		if (cblk) {
			cblk->bnr = bnr;
			cblk->hashed = 1;

			DTRACEF_CBLK("block_hash", cblk, "");

			htable_insert(inst->cache_ht, bnr, (u64)cblk);
			inst->nr_hashed++;
			get_cblk(cblk);

			clist_add_tail(&cblk->fifo_head, &inst->hot_fifo);
			get_cblk(cblk);

			try_shrink(inst);
		}
	}

	return cblk;
}

static bool should_submit(struct block_cache_instance *inst)
{
	return inst->nr_in_flight < inst->queue_depth && !list_empty(&inst->submit_list);
}

/*
 * We're processing the completion in the cqe callback itself, rather
 * than trying to hand it off to a utask.  It seems like it'd be cleaner
 * to have the work done in a utask (that could conceivably have
 * accounting), but it also seems like busywork for no functional gain.
 */
static void io_completion(struct io_uring_cqe *cqe, struct utask_cqe_callback *cb)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk = container_of(cb, struct cached_block, cb);
	int err;

	DTRACEF_CBLK("block_io_complete", cblk, "res %d", cqe->res);

	/* XXX error handling :) */
	BUG_ON(cqe->res != NGNFS_BLOCK_SIZE);
	err = 0;

	if (cblk->dirty) {
		if (--inst->nr_dirty_submitted <= 0)
			utask_wake_all(&inst->write_wq);
	} else {
		cblk->uptodate = 1;
		utask_wake_all(&cblk->wq);
	}

	cblk->queued = 0;
	cblk->error = err;
	put_cblk(cblk); /* submit list ref that covered io */
	cblk = NULL;

	inst->nr_in_flight--;
	if (should_submit(inst))
		utask_wake_task(inst->submit_tsk);

	/* catch block_exit shutting down */
	if (inst->exit_tsk && inst->nr_in_flight == 0)
		utask_wake_task(inst->exit_tsk);
}

/*
 * Prepare queued IO for submitting to io_uring.  The IO queue is
 * currently a simple list but this is where we'd add any sort of io
 * sorting/priority.
 */
static void submit_utask(void *data)
{
	struct block_cache_instance *inst = data;
	struct io_uring_sqe *sqe;
	struct cached_block *cblk;
	struct cached_block *tmp;
	int ret;

	for (;;) {
		ret = utask_wait_event_task(should_submit(inst));
		if (ret < 0)
			break;

		list_for_each_entry_safe(cblk, tmp, &inst->submit_list, submit_head) {
			if (inst->nr_in_flight >= inst->queue_depth)
				break;

			sqe = io_uring_get_sqe(utask_ring());
			if (!sqe)
				break;

			utask_set_sqe_callback(sqe, &cblk->cb, io_completion);
			if (cblk->dirty)
				io_uring_prep_write(sqe, inst->dev_fd,
						    page_address(cblk->data_page),
						    NGNFS_BLOCK_SIZE,
						    cblk->bnr << NGNFS_BLOCK_SHIFT);
			else
				io_uring_prep_read(sqe, inst->dev_fd,
						   page_address(cblk->data_page),
						   NGNFS_BLOCK_SIZE,
						   cblk->bnr << NGNFS_BLOCK_SHIFT);

			DTRACEF_CBLK("block_io_submit", cblk, "");

			inst->nr_in_flight++;
			list_del_init(&cblk->submit_head);
			/* submit list ref is transferred to io, put at completion */
		}
	}
}

static void queue_unless_uptodate(struct block_cache_instance *inst, struct cached_block *cblk)
{
	if (!cblk->error && !cblk->uptodate && !cblk->queued) {
		list_add_tail(&cblk->submit_head, &inst->submit_list);
		get_cblk(cblk);
		cblk->queued = 1;
		if (should_submit(inst))
			utask_wake_task(inst->submit_tsk);
	}
}

int block_alloc_pool(struct list_head *pool, size_t nr)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret = 0;
	size_t i = nr;

	while (i--) {
		cblk = alloc_cblk(inst);
		if (!cblk) {
			ret = -ENOMEM;
			break;
		}
		list_add_tail(&cblk->fifo_head, pool);
	}

	dtracef("block_alloc_pool", "pool %p nr %lu ret %d", pool, nr, ret);

	if (ret < 0)
		block_free_pool(pool);

	return ret;
}

void block_free_pool(struct list_head *pool)
{
	struct cached_block *cblk;
	size_t nr = 0;

	while ((cblk = del_first_pool_block(pool))) {
		put_cblk(cblk);
		nr++;
	}

	dtracef("block_free_pool", "pool %p freeing %lu unused cblks", pool, nr);
}

int block_lookup(u64 bnr, struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	int ret;

	*cblk_ret = lookup_cblk(inst, bnr);
	ret = *cblk_ret ? 0 : -ENOENT;

	DTRACEF_CBLK("block_lookup", *cblk_ret, "ret %d", ret);

	return ret;
}

int block_read_verify(u64 bnr, block_verify_fn_t verify_fn, void *verify_arg,
		      struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	cblk = lookup_or_alloc_cblk(inst, NULL, bnr);
	if (!cblk) {
		ret = -ENOMEM;
		goto out;
	}

	queue_unless_uptodate(inst, cblk);
	ret = utask_wait_event(&cblk->wq, cblk->uptodate || cblk->error);
	if (ret == 0)
		ret = cblk->error;
	if (ret < 0)
		goto out;

	if (verify_fn && !cblk->verified) {
		ret = verify_fn(cblk, verify_arg);
		if (ret < 0) {
			unhash_cblk(inst, cblk);
			goto out;
		}
		cblk->verified = 1;
	}

	ret = 0;
out:
	if (ret < 0 && cblk) {
		put_cblk(cblk);
		cblk = NULL;
	}

	DTRACEF_CBLK("block_read_verify", cblk, "ret %d", ret);

	*cblk_ret = cblk;
	return ret;
}

int block_read(u64 bnr, struct cached_block **cblk_ret)
{
	return block_read_verify(bnr, NULL, NULL, cblk_ret);
}

/*
 * Submit a read for the given block if it could be read, but don't
 * block waiting for it.  Does nothing if there's already a block that's
 * already uptodate or has been submitted for reading.
 */
void block_readahead(u64 bnr)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;

	cblk = lookup_or_alloc_cblk(inst, NULL, bnr);
	if (cblk) {
		queue_unless_uptodate(inst, cblk);
		put_cblk(cblk);
	}
}

/*
 * Return a reference to a dirty block.  If the caller doesn't provide a
 * data_page then the contents are unknown.  If an existing block
 * doesn't exist then a new block is allocated from the pool.
 *
 * If an existing block isn't already dirty then we don't want to modify
 * it under the caller.  We unhash it, leaving the caller's reference
 * intact, and try to allocate a new block to insert in its place.
 */
int block_create_dirty(u64 bnr, struct list_head *pool, struct page *data_page,
		       struct cached_block **cblk_ret)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	cblk = lookup_or_alloc_cblk(inst, pool, bnr);
	if (cblk && cblk->hashed && (cblk->uptodate || cblk->queued) && !cblk->dirty) {
		unhash_cblk(inst, cblk);
		put_cblk(cblk);
		cblk = lookup_or_alloc_cblk(inst, pool, bnr);
	}
	if (!cblk) {
		ret = -ENOMEM;
		goto out;
	}

	if (data_page) {
		if (cblk->data_page)
			put_page(cblk->data_page);
		cblk->data_page = data_page;
		get_page(cblk->data_page);
	}

	if (!cblk->dirty) {
		cblk->uptodate = 1;
		cblk->verified = 1;
		cblk->dirty = 1;
		cblk->error = 0;

		list_add_tail(&cblk->dirty_head, &inst->dirty_list);
		get_cblk(cblk);
		inst->nr_dirty++;
	}

	ret = 0;
out:
	if (ret < 0 && cblk) {
		put_cblk(cblk);
		cblk = NULL;
	}

	DTRACEF_CBLK("block_create_dirty", cblk, "ret %d", ret);

	*cblk_ret = cblk;
	return ret;
}

/*
 * Try and write all dirty blocks.  Returns once all submitted write IOs
 * are complete.
 *
 * The caller is responsible for ensuring that there is only ever one
 * caller at a time.
 */
int block_write_all_dirty(void)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	int ret;

	inst->write_err = 0;
	list_for_each_entry(cblk, &inst->dirty_list, dirty_head) {
		list_add_tail(&cblk->submit_head, &inst->submit_list);
		cblk->queued = 1;
		get_cblk(cblk);
	}
	inst->nr_dirty_submitted += inst->nr_dirty;

	if (should_submit(inst))
		utask_wake_task(inst->submit_tsk);

	ret = utask_wait_event(&inst->write_wq, inst->nr_dirty_submitted == 0);
	if (ret == 0)
		ret = inst->write_err;

	return ret;
}

/*
 * This is a separate call, instead of being done before
 * _write_all_dirty_returns, so that the caller can rely on getting
 * references to the pinned dirty blocks as they finish writing.  Once
 * done they can clean all the dirty blocks and make them reclaimable.
 */
void block_clean_all_dirty(void)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	struct cached_block *tmp;

	list_for_each_entry_safe(cblk, tmp, &inst->dirty_list, dirty_head) {
		list_del_init(&cblk->dirty_head);
		cblk->dirty = 0;
		put_cblk(cblk);
	}
	inst->nr_dirty = 0;
}

/*
 * Make the block no longer present in the cache.  Active users can
 * still have a reference to the block.  The last put will free it.
 */
void block_invalidate(struct cached_block *cblk)
{
	struct block_cache_instance *inst = &global_block_cache_inst;

	unhash_cblk(inst, cblk);
}

void *block_data_buf(struct cached_block *cblk)
{
	return page_address(cblk->data_page);
}

struct page *block_data_page(struct cached_block *cblk)
{
	return cblk->data_page;
}

void block_put(struct cached_block *cblk)
{
	if (cblk)
		put_cblk(cblk);
}

/*
 * A put that clears the pointer.
 */
void block_putp(struct cached_block **cblk)
{
	if (*cblk) {
		put_cblk(*cblk);
		*cblk = NULL;
	}
}

u64 block_total_blocks(void)
{
	struct block_cache_instance *inst = &global_block_cache_inst;

	return inst->total_blocks;
}

int block_init(char *dev_path, unsigned long queue_depth)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	int oflags;
	u64 size;
	int ret;

	if (WARN_ON_ONCE(queue_depth == 0))
		return -EINVAL;

	oflags = O_RDWR | O_DIRECT;
	inst->dev_fd = open(dev_path, oflags, O_RDWR);
	if (inst->dev_fd < 0 && errno == EINVAL) {
		oflags &= ~O_DIRECT;
		errno = 0;
		inst->dev_fd = open(dev_path, oflags, O_RDWR);
		if (inst->dev_fd >= 0)
			log("O_DIRECT not supported on '%s', using buffered", dev_path);
	}
	if (inst->dev_fd < 0) {
		ret = -errno;
		log("error opening device '%s' :" ENOF, dev_path, ENOA(-ret));
		goto out;
	}
	inst->queue_depth = queue_depth;

	ret = devfd_get_size(inst->dev_fd, &size);
	if (ret < 0)
		goto out;

	inst->total_blocks = size / NGNFS_BLOCK_SIZE;

	inst->cache_ht = htable_alloc(MAX_CACHED_BLOCKS);
	if (!inst->cache_ht) {
		ret = -ENOMEM;
		goto out;
	}

	ret = utask_create(submit_utask, inst, &inst->submit_tsk);
out:
	if (ret < 0)
		block_exit();
	return ret;
}

void block_exit(void)
{
	struct block_cache_instance *inst = &global_block_cache_inst;
	struct cached_block *cblk;
	struct cached_block *tmp;
	unsigned long fe;
	int ret;

	utask_cancel(inst->submit_tsk);

	/* wait for io to drain, completion tries to wake submit */
	inst->exit_tsk = utask_current();
	ret = utask_wait_event_task(inst->nr_in_flight == 0);
	BUG_ON(ret != 0); /* _exit shouldn't be canceled */
	inst->exit_tsk = NULL;

	utask_destroy(inst->submit_tsk);
	inst->submit_tsk = NULL;

	if (inst->dev_fd >= 0) {
		close(inst->dev_fd);
		inst->dev_fd = -1;
	}

	block_clean_all_dirty();

	list_for_each_entry_safe(cblk, tmp, &inst->submit_list, submit_head)
		put_cblk(cblk);

	while ((cblk = clist_del_first_past_pct(inst, &inst->hot_fifo, 0) ?:
		       clist_del_first_past_pct(inst, &inst->cold_fifo, 0))) {
		put_cblk(cblk);
	}

	if (inst->cache_ht) {
		htable_foreach_init(inst->cache_ht, &fe);
		while ((cblk = (struct cached_block *)htable_foreach(inst->cache_ht, &fe)))
			unhash_cblk(inst, cblk);
		free(inst->cache_ht);
		inst->cache_ht = NULL;
	}
}
