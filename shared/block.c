/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/atomic.h"
#include "shared/lk/barrier.h"
#include "shared/lk/bitops.h"
#include "shared/lk/bug.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/cmpxchg.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"
#include "shared/lk/llist.h"
#include "shared/lk/minmax.h"
#include "shared/lk/processor.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/rhashtable.h"
#include "shared/lk/wait.h"
#include "shared/lk/workqueue.h"

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/map.h"
#include "shared/msg.h"
#include "shared/string_wrappers.h"
#include "shared/trace.h"

/*
 * This client block cache coordinates block state between transaction
 * users and network protocol communication with the devd device
 * servers.
 *
 * Cached blocks are tracked in an rcu hash table.  Transaction users
 * get read and write references to access and modify block contents.
 * Read references are shared with readers and a single write reference
 * is exclusive.
 *
 * The ability for the cache to give read and write references to
 * callers is governed by cache mode state given by the devd servers.
 * _get callers request a given mode when sending a read message to get
 * block contents.  The server specifies the allowed mode when
 * responding with the block data.  _get callers can then request
 * additional modes when the initially given mode isn't sufficient.  At
 * any point, the server can send a message removing the allowed mode.
 * The cache has to acknowledge these restrictions, but only after
 * existing _get refences finish.
 *
 * Write references leave behind dirty blocks, which are later written
 * by sending them to the devd servers.  Read references are allowed
 * while blocks are being written but writes are excluded.
 *
 * There are some very simple thresholds on the number of dirty blocks
 * at which start background flushing writes or limit the total number
 * of dirty blocks.
 *
 * Today there are no atomic write transactions.  Dirty blocks are added
 * to a flush list and are written, and can fail, independently.  This
 * will change as the network protocol adds support for distributed
 * transactions.  The cache will need to track atomic units of dirty
 * blocks and hand them to the transport which can describe the group in
 * write messages sent over the wire.
 *
 * When implementing invalidations and atomic writes, there's an
 * irritating deadlock to address.  Transactions are tracking their
 * block references so they can use trylock semantics to acquire
 * references (and request access over the network) out of order and be
 * asked to retry in order when they hit a potential deadlock. Write
 * references might pin entire existing dirty atomic block sets, not
 * just individual blocks.  This effectively means that transactions are
 * also holding references to all the blocks in the dirty set, without
 * knowing it.  They could block on a reference in order with their
 * transaction blocks but out of order with respect to the dirty blocks
 * that are pinned.  (One fix might be to have invalidations wake
 * blocked reference acquisitions that pin the set.  We'll see what
 * seems least awful.)
 */

/*
 * Callers will wait to enter their write transaction until the number
 * of dirty blocks falls below this limit.  They can exceed the limit
 * while they're dirtying blocks in their transaction.
 */
#define DIRTY_LIMIT	1024

/*
 * Background flushing will start when the number of dirty blocks exceeds this
 * threshold.
 */
#define FLUSH_THRESH	256

/*
 * Stages in the block processing pipeline gather inputs from multiple
 * producers on a lockless list for a single work consumer to manage
 * with a private list_head list.
 */
struct block_work_list {
	struct llist_head llist;
	struct list_head list;
	struct workqueue_struct *wq;
	struct work_struct work;
};

struct ngnfs_block_info {
	struct ngnfs_fs_info *nfi;
	struct rhashtable ht;

	int queue_depth;
	atomic_t nr_dirty;
	atomic_t nr_flushing;
	atomic_t nr_mode_flushing;
	atomic_t nr_submitted;
	atomic_t sync_waiters;

	struct block_work_list flush;
	struct block_work_list submit;
	struct block_work_list clean;

	wait_queue_head_t waitq;
};

/* XXX hmm, should this be public? */
#define ST_CACHE_MODE_BITS 2
#define ST_READERS_BITS 20

/*
 * We want atomic transitions between combinations of multiple block
 * states.  Some examples include wanting to set a new cache mode while
 * there are no conflicting readers or writers, or wanting to mark a
 * block to ack setting a cache mode while writing if a block is dirty
 * but not if it's already being flushed.
 *
 * We could protect all this with lock/modify/unlock patterns, but
 * instead we use atomic prepare/apply patterns which have a more
 * friendly shared_load/atomic_store contention profile.
 *
 * Sorry that this definition's indentation stinks.  We want to use the
 * bare bitfield identifiers, without noisy prefixes, while also
 * operating on them atomically with the overlapping long.  We also
 * don't want the definitions to smash up against the margin so.. here
 * we are.
 */
struct ngnfs_block_state {
	union {
		unsigned long _state;
		struct {
			unsigned long

	/*
	 * Multiple shared readers hold references, excluding a writer.
	 */
	readers:ST_READERS_BITS,

	/*
	 * A writer holds a exclusive reference.
	 */
	writer:1,

	/*
	 * The block has modified contents.  It must be flushed before
	 * being removed from the cache.
	 */
	dirty:1,

	/*
	 * Either a block _read or _write message is in flight.  The
	 * setter is responsible for putting the block on the submit
	 * list.  Cleared when another r/w can be submitted.  For reads
	 * after the read result, but for writes after the cleaning work
	 * is done (which can be tighened up once we get rid of the
	 * dirty/clean lists).
	 */
	io_pending:1,

	/*
	 * The block data is consistent with the current persistent
	 * version.  The cached data can be read or updated.
	 */
	uptodate:1,

	/*
	 * An IO error has occurred.  The specific negative errno is
	 * found in err.
	 */
	error:1,

	/*
	 * The block is tracking a sync waiter and only exists on the
	 * flush and clean lists.  It's removed from the lists when all
	 * the io before it on the lists has completed, which wakes the
	 * waiter.
	 */
	sync_waiter:1,

	/*
	 * Cache mode controls.  mode is the current active mode that
	 * the block can provide to users.  set_mode is the next mode
	 * that we've been asked to set by an incoming message and
	 * _setting tracks a mode set in progress.  _acking and
	 * _flushing are exclusive and give the caller the
	 * responsibility of sending an ack message or including the ack
	 * mode in a flushing write, respectively.
	 */
	mode:ST_CACHE_MODE_BITS,
	set_mode:ST_CACHE_MODE_BITS,
	cache_mode_setting:1,
	cache_mode_acking:1,
	cache_mode_flushing:1,

	/*
	 * Lets _get atomically set arguments that read submission puts
	 * in the network message.
	 */
	read_mode:ST_CACHE_MODE_BITS,
	read_no_data:1;

		};
	};
};

/*
 * Helpful for manual debugging.
 */
#define STF "%c%c%c%c%c%c%c r %u md %u rmd %u smd %u"

#define STC(X, C) ((X) ? C : '-')
#define STA(st)				\
	STC((st)->writer, 'w'),		\
	STC((st)->dirty, 'd'),		\
	STC((st)->io_pending, 'i'),	\
	STC((st)->uptodate, 'u'),	\
	STC((st)->error, 'e'),		\
	STC((st)->sync_waiter, 's'),	\
	STC((st)->read_no_data, 'r'),	\
	(st)->readers,			\
	(st)->mode,			\
	(st)->read_mode,		\
	(st)->set_mode

struct ngnfs_block {
	atomic_t refcount;
	struct ngnfs_block_state st;
	struct rcu_head rcu;
	struct rhash_head rhead;
	struct llist_node dirty_llnode;
	struct list_head dirty_head;
	struct llist_node submit_llnode;
	struct list_head submit_head;
	wait_queue_head_t waitq;
	int err;
	struct page *page;
	u64 bnr;
};

/*
 * Return the result of an expression in terms of an atomic read of the
 * state which has _acquire semantics.
 */
#define st_expr(BL_, ST_, EXPR_)		\
({						\
	smp_rmb();				\
	*(ST_) = READ_ONCE((BL_)->st);		\
	(EXPR_);				\
})

/*
 * Prepare for an attempt to change block state.  As this returns old ==
 * st, the caller can modify st and old will be used to try apply the
 * changes in st with cmpxchg.
 */
static void st_change_prepare(struct ngnfs_block *bl, struct ngnfs_block_state *old,
			      struct ngnfs_block_state *st)
{
	smp_rmb(); /* acquire */
	*old = READ_ONCE(bl->st);
	*st = *old;
}

/*
 * Attempt to change block state.  The caller prepared old and modified
 * st.  If this returns true then old and st represent the state before
 * and after the caller's change, respectively.  If this returns false
 * then old and new are both set to the current state and the caller can
 * attempt to change the state again.
 */
static bool st_change_apply(struct ngnfs_block *bl, struct ngnfs_block_state *old,
			    struct ngnfs_block_state *st)
{
	unsigned long _state = cmpxchg(&bl->st._state, old->_state, st->_state);

	if (_state == old->_state) {
		smp_wmb(); /* release */
		return true;
	}

	old->_state = _state;
	*st = *old;

	return false;
}

/*
 * A simpler wrapper for prepare/apply for changes with simple cond and
 * change expressions.  Returns the boolean result of evaluating the
 * condition which will indicate if change was made.  This evaluates all
 * its arguments multiple times.
 */
#define st_cond_change(BL_, OLD_, ST_, COND_, EXPR_)			\
({									\
	bool ret_;							\
									\
	st_change_prepare((BL_), (OLD_), (ST_));			\
	do {								\
		ret_ = (COND_);						\
		if (!ret_)						\
			break;						\
		(EXPR_);						\
	} while (!(ret_ = st_change_apply((BL_), (OLD_), (ST_))));	\
									\
	ret_;								\
})

/*
 * And the simplest change which unconditionally modifies state.  (And
 * always returns true.  I'm not sure that it should bother.)
 */
#define st_change(BL_, ST_, EXPR_)				\
({								\
	struct ngnfs_block_state old_;				\
								\
	st_cond_change((BL_), &old_, (ST_), true, (EXPR_));	\
})

/* declaring these here so that we can have their wake condition along side their work */
static void try_queue_flush_work(struct ngnfs_block_info *blinf);
static void try_queue_submit_work(struct ngnfs_block_info *blinf);
static void queue_clean_work(struct ngnfs_block_info *blinf);

static void init_block_work_list(struct block_work_list *worklist, work_func_t func)
{
	init_llist_head(&worklist->llist);
	INIT_LIST_HEAD(&worklist->list);
	worklist->wq = NULL;
	INIT_WORK(&worklist->work, func);
}

static void destroy_block_work_list(struct block_work_list *worklist)
{
	if (worklist->wq)
		destroy_workqueue(worklist->wq);
}

static void free_block(struct ngnfs_block *bl)
{
	if (!IS_ERR_OR_NULL(bl)) {
		BUG_ON(waitqueue_active(&bl->waitq));

		if (bl->page)
			put_page(bl->page);
		kfree(bl);
	}
}

static struct ngnfs_block *alloc_block(u64 bnr, bool with_page)
{
	struct ngnfs_block *bl;

	/* should know how to alloc sub pages */
	BUILD_BUG_ON(NGNFS_BLOCK_SIZE < PAGE_SIZE);

	bl = kzalloc(sizeof(struct ngnfs_block), GFP_NOFS);
	if (bl) {
		atomic_set(&bl->refcount, 1);
		init_llist_node(&bl->dirty_llnode);
		init_llist_node(&bl->submit_llnode);
		INIT_LIST_HEAD(&bl->submit_head);
		init_waitqueue_head(&bl->waitq);

		if (with_page)
			bl->page = alloc_page(GFP_NOFS);
		bl->bnr = bnr;
	}

	if (!bl || (with_page && !bl->page)) {
		free_block(bl);
		bl = ERR_PTR(-ENOMEM);
	}

	return bl;
}

static void free_block_rcu(struct rcu_head *rcu)
{
	struct ngnfs_block *bl = container_of(rcu, struct ngnfs_block, rcu);

	free_block(bl);
}

static void get_block(struct ngnfs_block *bl)
{
	int now = atomic_inc_return(&bl->refcount);

	trace_ngnfs_block_inc_refcount((long long)bl, now);
	BUG_ON(now <= 0);
}

static void put_block(struct ngnfs_block *bl)
{
	int now = 0;

	if (!IS_ERR_OR_NULL(bl)) {
		now = atomic_dec_return(&bl->refcount);
		trace_ngnfs_block_dec_refcount((long long)bl, now);
		if (now == 0)
			call_rcu(&bl->rcu, free_block_rcu);
		else
			BUG_ON(now < 0);
	}
}

/*
 * We use an atomic to record any io errors for the tasks that are in
 * sync.  We use the low bit to indicate error and assume that we'll
 * never have enough waiters to overflow.
 */
#define SYNC_WAITERS_ERR 1
#define SYNC_WAITERS_INC 2

static void sync_waiters_inc(struct ngnfs_block_info *blinf)
{
	atomic_add(SYNC_WAITERS_INC, &blinf->sync_waiters);
}

static void sync_waiters_set_error(struct ngnfs_block_info *blinf)
{
	int old;

	do {
		old = atomic_read(&blinf->sync_waiters);
	} while ((old >= SYNC_WAITERS_INC) && !(old & SYNC_WAITERS_ERR) &&
		 (atomic_cmpxchg(&blinf->sync_waiters, old, old | SYNC_WAITERS_ERR) != old));
}

static bool sync_waiters_has_error(struct ngnfs_block_info *blinf)
{
	return !!(atomic_read(&blinf->sync_waiters) & SYNC_WAITERS_ERR);
}

/*
 * Decrement the caller's previous increment of sync_waiters, returning
 * -EIO if there was an error while they were waiting, and clearing the
 * error if they were the last waiter.
 */
static int sync_waiters_dec_error(struct ngnfs_block_info *blinf)
{
	int ret = 0;
	int old;
	int new;

	do {
		old = atomic_read(&blinf->sync_waiters);
		ret = (old & SYNC_WAITERS_ERR) ? -EIO : 0;
		new = old - SYNC_WAITERS_INC;
		if (new == SYNC_WAITERS_ERR)
			new = 0;

	} while (atomic_cmpxchg(&blinf->sync_waiters, old, new) != old);

	return ret;
}

static int send_to_bnr(struct ngnfs_fs_info *nfi, u64 bnr, struct ngnfs_msg_desc *mdesc)
{
	struct sockaddr_in addr;
	int ret;

	ret = ngnfs_map_map_block(nfi, bnr, &addr);
	if (ret == 0) {
		mdesc->addr = &addr;
		ret = ngnfs_msg_send(nfi, mdesc);
	}

	return ret;
}

static const struct rhashtable_params ngnfs_block_ht_params = {
        .head_offset = offsetof(struct ngnfs_block, rhead),
        .key_offset = offsetof(struct ngnfs_block, bnr),
        .key_len = sizeof_field(struct ngnfs_block, bnr),
};

static struct ngnfs_block *lookup_block(struct ngnfs_block_info *blinf, u64 bnr)
{
	struct ngnfs_block *bl;

	rcu_read_lock();
	bl = rhashtable_lookup(&blinf->ht, &bnr, ngnfs_block_ht_params);
	if (bl)
		get_block(bl);
	rcu_read_unlock();

	return bl;
}

/*
 * Returns a block with a reference held or an ERR_PTR on allocation
 * failure or lookup that won't allocate.
 */
static struct ngnfs_block *lookup_or_alloc_block(struct ngnfs_block_info *blinf, u64 bnr)
{
	struct ngnfs_block *found;
	struct ngnfs_block *bl;

	bl = lookup_block(blinf, bnr);
	if (!bl) {
		bl = alloc_block(bnr, true);
		if (!IS_ERR(bl)) {
			get_block(bl);
			rcu_read_lock();
			found = rhashtable_lookup_get_insert_fast(&blinf->ht, &bl->rhead,
								  ngnfs_block_ht_params);
			if (found) {
				put_block(bl);
				put_block(bl);
				bl = found;
				get_block(bl);
			}
			rcu_read_unlock();
		}
	}

	return bl;
}

static int send_block_read(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl)
{
	struct ngnfs_msg_block_read br;
	struct ngnfs_msg_desc mdesc;
	struct ngnfs_block_state st;

	br.bnr = cpu_to_le64(bl->bnr);
	br.flags = 0;
	if (st_expr(bl, &st, st.read_no_data))
		br.flags |= cpu_to_le64(NGNFS_MSG_BLOCK_READ_FLAG_NO_DATA);
	br.mode = st.read_mode;
	memset_zero_sizeof(br._pad);

	mdesc.ctl_buf = &br;
	mdesc.ctl_size = sizeof(br);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;
	mdesc.type = NGNFS_MSG_BLOCK_READ;

	return send_to_bnr(nfi, bl->bnr, &mdesc);
}

/*
 * If w're writing in response to an incoming cache mode request then we
 * might be acknowledging the new mode along with the write message.  If
 * not, the set will complete and an explicit ack will be sent once the
 * write is complete.
 */
static int send_block_write(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl)
{
	struct ngnfs_msg_block_write bw;
	struct ngnfs_msg_desc mdesc;
	struct ngnfs_block_state st;

	bw.bnr = cpu_to_le64(bl->bnr);
	if (st_expr(bl, &st, st.cache_mode_flushing))
		bw.mode = st.mode;
	else
		bw.mode = NGNFS_CACHE_MODE_NULL;
	memset_zero_sizeof(bw._pad);

	mdesc.ctl_buf = &bw;
	mdesc.ctl_size = sizeof(bw);
	mdesc.data_page = bl->page;
	mdesc.data_size = NGNFS_BLOCK_SIZE;
	mdesc.type = NGNFS_MSG_BLOCK_WRITE;

	return send_to_bnr(nfi, bl->bnr, &mdesc);
}

/*
 * Send an explicit ack of having set the mode that the server
 * requested.
 */
static int send_block_mode_ack(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl)
{
	struct ngnfs_msg_cache_mode cm;
	struct ngnfs_msg_desc mdesc;
	struct ngnfs_block_state st;

	cm.bnr = cpu_to_le64(bl->bnr);
	cm.mode = st_expr(bl, &st, st.mode);
	memset_zero_sizeof(cm._pad);

	mdesc.ctl_buf = &cm;
	mdesc.ctl_size = sizeof(cm);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;
	mdesc.type = NGNFS_MSG_BLOCK_MODE_ACK;

	return send_to_bnr(nfi, bl->bnr, &mdesc);
}

/*
 * Called from within a change attempt.  The server must only send us
 * one set at a time.
 */
static void start_set_cache_mode(struct ngnfs_block_state *st, u8 mode)
{
	BUG_ON(st->cache_mode_setting); /* XXX should be error handling */

	st->cache_mode_setting = 1;
	st->set_mode = mode;
}

static void finish_set_cache_mode(struct ngnfs_block *bl)
{
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;

	st_change_prepare(bl, &old, &st);
	do {
		st.cache_mode_setting = 0;
		st.cache_mode_acking = 0;
		st.cache_mode_flushing = 0;
		st.set_mode = NGNFS_CACHE_MODE_NULL;

	} while (!st_change_apply(bl, &old, &st));
}

/*
 * The caller has made changes to block state that let a pending setting
 * of the cache mode make forward progress.  First we have to wait for
 * current users that are incompatible with the new mode to finish.
 *
 * Once that's done, there's three broad ways for us to send an
 * acknowledgement back to the server.  If the block is dirty, and isn't
 * flushing, then we can initiate flushing which can also serve as the
 * ack.  If the block is dirty, and undergoing flushing, then we just
 * wait for the write to complete and clean the block.  For clean
 * blocks, we send an explicit ack.
 *
 * In the common case this will be called when there isn't a mode set
 * pending and it will return after sampling the block state.
 */
static void try_set_cache_mode(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	bool incremented = false;
	int ret;

	st_change_prepare(bl, &old, &st);
	do {
		/* done if we're not changing the mode */
		if (!st.cache_mode_setting)
			return;

		/* have to wait for conflicting references to drain */
		if ((st.set_mode < NGNFS_CACHE_MODE_WRITE && st.writer) ||
		    (st.set_mode < NGNFS_CACHE_MODE_READ && st.readers))
			return;

		/* adopt the new mode before sending the ack */
		if (st.mode != st.set_mode)
			st.mode = st.set_mode;

		if (st.dirty && !st.io_pending && !st.cache_mode_flushing) {
			/*
			 * Marking dirty blocks cache_mode_flushing
			 * before IO starts lets the write message
			 * indicate the ack.
			 */
			if (!incremented) {
				atomic_dec(&blinf->nr_mode_flushing);
				incremented = true;
			}
			st.cache_mode_flushing = 1;

		} else if (!st.cache_mode_flushing && !st.cache_mode_acking) {
			/*
			 * Otherwise we send an explicit ack manually
			 * for clean blocks.
			 */
			st.cache_mode_acking = 1;
		}

	} while (!st_change_apply(bl, &old, &st));

	if (!old.cache_mode_flushing && st.cache_mode_flushing) {
		/* queue flushing if we marked _mode_flushing and inced nr_mode_flushing */
		try_queue_flush_work(blinf);
	} else if (incremented) {
		/* drop the increment that we didn't end up needing */
		atomic_dec(&blinf->nr_mode_flushing);
	}

	if (!old.cache_mode_acking && st.cache_mode_acking) {
		/* send the ack if we set _acking */
		ret = send_block_mode_ack(nfi, bl);
		BUG_ON(ret != 0); /* XXX */

		finish_set_cache_mode(bl);

	} else if (!st.dirty && !st.cache_mode_acking && st.cache_mode_flushing) {
		/* and we're finished when write completed with our implicit ack */
		finish_set_cache_mode(bl);
	}
}

/*
 * Update our local cached block with the incoming result of our read
 * request.  It may not contain block data if, for example, we're
 * requesting write mode and are going to overwrite the current data
 * (NBF_NEW -> _NO_DATA).
 */
static int recv_block_read_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_msg_block_read_result *rr = mdesc->ctl_buf;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	struct ngnfs_block *bl;
	int err;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_block_read_result) ||
	    (!!mdesc->data_size != !!mdesc->data_page) ||
	    ((mdesc->data_size != 0) && (mdesc->data_size != NGNFS_BLOCK_SIZE)) ||
	    ((rr->err != NGNFS_MSG_ERR_OK) && (mdesc->data_size != 0)))
		return -EINVAL;

	bl = lookup_block(blinf, le64_to_cpu(rr->bnr));
	assert(!IS_ERR_OR_NULL(bl)); /* not supporting this failure yet */

	err = ngnfs_msg_errno(rr->err);

	/* set error before clearing io_pending */
	if (err && st_cond_change(bl, &old, &st, !st.error, st.error = 1))
		bl->err = err;

	/* replace current block data with what we just received */
	if (!err && mdesc->data_page) {
		/* this means that _block_buf() will change, callers beware */
		if (bl->page)
			put_page(bl->page);
		bl->page = mdesc->data_page;
		get_page(bl->page);
	}

	/* clear pending, users will proceed once this is stored */
	st_change_prepare(bl, &old, &st);
	do {
		if (!err) {
			if (mdesc->data_page)
				st.uptodate = 1;
			start_set_cache_mode(&st, rr->mode);
		}
		st.io_pending = 0;

	} while (!st_change_apply(bl, &old, &st));

	atomic_dec(&blinf->nr_submitted);

	barrier(); /* RELEASE: all block updates visible before we queue/wake */

	try_set_cache_mode(nfi, bl);

	wake_up(&bl->waitq);
	put_block(bl);

	return 0;
}

static int recv_block_write_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_msg_block_write_result *wr = mdesc->ctl_buf;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	struct ngnfs_block *bl;
	int nr_dirty;
	int err;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_block_write_result) ||
	    mdesc->data_size != 0)
		return -EINVAL;

	/* XXX describe trying page granular pinning */

	bl = lookup_block(blinf, le64_to_cpu(wr->bnr));
	assert(!IS_ERR_OR_NULL(bl)); /* XXX not supporting this failure yet */
	BUG_ON(st_expr(bl, &st, !st.dirty)); /* XXX must have been dirty */

	err = ngnfs_msg_errno(wr->err);
	if (err) {
		if (st_cond_change(bl, &old, &st, !st.error, st.error = 1))
			bl->err = err;
		sync_waiters_set_error(blinf);
	}

	/* updating accounting here, clean work puts reader */
	nr_dirty = atomic_dec_return(&blinf->nr_dirty);
	atomic_dec(&blinf->nr_submitted);

	/* clear dirty for cleaner, it will clear io_pending */
	st_change(bl, &st, st.dirty = 0);

	try_set_cache_mode(nfi, bl);

	wake_up(&bl->waitq);
	put_block(bl);

	try_queue_flush_work(blinf);
	queue_clean_work(blinf);
	if (nr_dirty < DIRTY_LIMIT && waitqueue_active(&blinf->waitq))
		wake_up(&blinf->waitq);

	return 0;
}

static int recv_block_mode_set(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_msg_cache_mode *cm = mdesc->ctl_buf;
	struct ngnfs_block_state st;
	struct ngnfs_block *bl;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_cache_mode) ||
	    mdesc->data_size != 0)
		return -EINVAL;

	/* XXX describe trying page granular pinning */

	bl = lookup_block(blinf, le64_to_cpu(cm->bnr));
	assert(!IS_ERR_OR_NULL(bl)); /* not supporting this failure yet */

	st_change(bl, &st, start_set_cache_mode(&st, cm->mode));

	try_set_cache_mode(nfi, bl);
	wake_up(&bl->waitq);
	put_block(bl);

	return 0;
}

/*
 * Callers are gathering items that were concurrently prepended to a
 * lockless list and are putting them on their private list_head list.
 * We preserve list order (for sync especially) so we walk the llist
 * lifo and construct a private fifo that is then spliced onto the end
 * of the caller's existing list.
 *
 * The lockless list is destroyed and its nodes are re-initialized as we
 * go.  The caller is only using the nodes to get the item on their
 * private list.  They can then add the items to other lockless lists as
 * needed.
 */
static void del_all_reverse_add_tail(struct list_head *list, struct llist_head *llist,
				     ssize_t offset)
{
	struct llist_node *first;
	struct llist_node *node;
	struct llist_node *n;
	struct list_head *head;
	LIST_HEAD(reverse);

	first = llist_del_all(llist);
	if (first) {
		llist_for_each_safe(node, n, first) {
			head = (void *)node + offset;
			init_llist_node(node);
			list_add(head, &reverse);
		}
		list_splice_tail(&reverse, list);
	}
}

/*
 * XXX barriers?
 */
static void try_queue_submit_work(struct ngnfs_block_info *blinf)
{
	if ((!list_empty(&blinf->submit.list) || !llist_empty(&blinf->submit.llist)) &&
	    (atomic_read(&blinf->nr_submitted) < blinf->queue_depth))
		queue_work(blinf->submit.wq, &blinf->submit.work);
}

/*
 * The submit work is responsible for keeping the transport's queue
 * depth full.
 */
static void ngnfs_block_submit_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info, submit.work);
	struct ngnfs_fs_info *nfi = blinf->nfi;
	struct ngnfs_block_state st;
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	int submitted;
	int space;
	int ret;

	del_all_reverse_add_tail(&blinf->submit.list, &blinf->submit.llist,
				 offsetof(struct ngnfs_block, submit_head) -
				 offsetof(struct ngnfs_block, submit_llnode));

	space = blinf->queue_depth - atomic_read(&blinf->nr_submitted);
	submitted = 0;

	list_for_each_entry_safe(bl, tmp, &blinf->submit.list, submit_head) {
		if (submitted == space)
			break;

		list_del_init(&bl->submit_head);

		if (st_expr(bl, &st, st.dirty))
			ret = send_block_write(nfi, bl);
		else
			ret = send_block_read(nfi, bl);
		BUG_ON(ret != 0);

		submitted++;
	}

	if (submitted)
		atomic_add(submitted, &blinf->nr_submitted);
}

/*
 * We should start flushing when:
 *
 * There is room on the IO queue to submit more IOs, AND one of the
 * following is true:
 *
 *  - the flushing list is non-empty
 *  - the number of dirty blocks is above the dirty threshold
 *  - there is a synchronous waiter
 *  - dirty blocks need to be flushed to ack incompatible cache mode
 *
 * Anything on the flush list has a read reference and can't be written,
 * so it's important to get it submitted ASAP.
 *
 * We can flush a bit too much if we're flushing blocks on behalf of
 * having to set incompatible cache modes.  When we move to tracking
 * dependent sets of dirty blocks this will pivot to be in terms of
 * sets, rather than blocks, so we're not going to invest in more
 * precise complexity until then.
 *
 * XXX barriers?
 */
static int should_flush(struct ngnfs_block_info *blinf)
{
	int dirty = atomic_read(&blinf->nr_dirty);
	int flushing = atomic_read(&blinf->nr_flushing);
	int submitted = atomic_read(&blinf->nr_submitted);
	/* Avoid submitting one block at a time */
	int depth = blinf->queue_depth / 2;

	if ((submitted < depth) &&
	    (flushing ||
	     (dirty > FLUSH_THRESH) ||
	     (atomic_read(&blinf->sync_waiters) >= SYNC_WAITERS_INC) ||
	     (atomic_read(&blinf->nr_mode_flushing) > 0)))
		return min(dirty, depth - submitted);
	else
		return 0;
}

static void try_queue_flush_work(struct ngnfs_block_info *blinf)
{
	if (should_flush(blinf))
		queue_work(blinf->flush.wq, &blinf->flush.work);
}

/*
 * The flush work is responsible for submitting write IO for dirty
 * blocks on the flush list.  Today the blocks are passed straight
 * through to the submit work.
 *
 * As write transactions are implemented this will be responsible for
 * grouping the blocks into transaction fragments and for resending if
 * the maps change.
 */
static void ngnfs_block_flush_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info,
						      flush.work);
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	bool submitted = false;
	int should;

	/* always gather dirtied sets from llist for iteration */
	del_all_reverse_add_tail(&blinf->flush.list, &blinf->flush.llist,
				 offsetof(struct ngnfs_block, dirty_head) -
				 offsetof(struct ngnfs_block, dirty_llnode));

	should = should_flush(blinf);
	BUG_ON(should < 0);

	list_for_each_entry_safe(bl, tmp, &blinf->flush.list, dirty_head) {

		if (st_expr(bl, &st, st.sync_waiter)) {
			list_del_init(&bl->dirty_head);
			llist_add(&bl->dirty_llnode, &blinf->clean.llist);
			queue_clean_work(blinf);
			submitted = true;
			continue;
		}

		if (should-- == 0)
			break;

		/* all flushing pauses until writers finish with next dirtied block */
		if (!st_cond_change(bl, &old, &st, !st.io_pending && !st.writer,
						   st.io_pending = 1))
			break;

		/* drop count of flushing so we stop once we've seen them all */
		if (st.cache_mode_flushing)
			atomic_dec(&blinf->nr_mode_flushing);

		list_del_init(&bl->dirty_head);
		get_block(bl);
		atomic_inc(&blinf->nr_flushing);

		barrier(); /* release: order updates before visible on lists */

		llist_add(&bl->dirty_llnode, &blinf->clean.llist);
		llist_add(&bl->submit_llnode, &blinf->submit.llist);

		submitted = true;
	}

	if (submitted || should)
		try_queue_submit_work(blinf);

}

static void queue_clean_work(struct ngnfs_block_info *blinf)
{
	queue_work(blinf->clean.wq, &blinf->clean.work);
}

/*
 * The clean work's only job is to walk the clean list and remove blocks
 * whose write IO has completed.  We defer this until after (possibly
 * out of order) io completion so that we can put sync waiters in the
 * flush->clean list and wake them once all the write IO they're waiting
 * for is complete.
 */
static void ngnfs_block_clean_work(struct work_struct *work)
{
	struct ngnfs_block_info *blinf = container_of(work, struct ngnfs_block_info, clean.work);
	struct ngnfs_fs_info *nfi = blinf->nfi;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	struct ngnfs_block *tmp;
	struct ngnfs_block *bl;
	bool cleaned = false;

	del_all_reverse_add_tail(&blinf->clean.list, &blinf->clean.llist,
				 offsetof(struct ngnfs_block, dirty_head) -
				 offsetof(struct ngnfs_block, dirty_llnode));

	list_for_each_entry_safe(bl, tmp, &blinf->clean.list, dirty_head) {

		st_change_prepare(bl, &old, &st);
		do {
			if (st.dirty)
				goto out;

			if (st.sync_waiter)
				st.sync_waiter = 0;
			else
				st.io_pending = 0;

		} while (!st_change_apply(bl, &old, &st));

		list_del_init(&bl->dirty_head);
		cleaned = true;

		if (old.io_pending)
			atomic_dec(&blinf->nr_flushing);

		try_set_cache_mode(nfi, bl);

		wake_up(&bl->waitq);
		put_block(bl);
	}

out:
	if (cleaned)
		try_queue_flush_work(blinf);
}

static u8 cache_mode_from_nbf(nbf_t nbf)
{
	return (nbf & NBF_READ) ? NGNFS_CACHE_MODE_READ :
	       (nbf & NBF_WRITE) ? NGNFS_CACHE_MODE_WRITE :
				   NGNFS_CACHE_MODE_NULL;
}

/*
 * Returns true if a caller with the given get flags can't increment
 * readers/writer given the current users of the block.  Straight
 * forward, except for the weird case of a converting writer promising
 * that it has a read ref and so being able to convert if it's the only
 * reader.
 */
static bool conflicting_rw_refs(struct ngnfs_block_state *st, nbf_t nbf)
{
	return st->writer ||
	       ((nbf & NBF_WRITE) &&
		((nbf & NBF_CONVERT_WRITE) ? st->readers != 1 : st->readers > 0));
}

static bool bad_nbf(nbf_t nbf)
{
	return ((nbf & NBF_READ) && (nbf & NBF_WRITE)) ||
	       ((nbf & (NBF_NEW | NBF_NODIRTY | NBF_CONVERT_WRITE)) && !(nbf & NBF_WRITE));
}

/*
 * Acquire a reference to a cached block.  The behaviour of the
 * reference is controlled by the flags as documented at the nbf_t
 * definition.  Successfully acquired references must later be released
 * by calling _put().
 */
struct ngnfs_block *ngnfs_block_get(struct ngnfs_fs_info *nfi, u64 bnr, nbf_t nbf)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block *bl = NULL;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	bool need_read;
	bool acquired;
	u8 get_mode;
	int ret;

	if (WARN_ON_ONCE(bad_nbf(nbf))) {
		ret = -EINVAL;
		goto out;
	}

	bl = lookup_or_alloc_block(blinf, bnr);
	if (IS_ERR(bl)) {
		ret = PTR_ERR(bl);
		goto out;
	}

	get_mode = cache_mode_from_nbf(nbf);

	for (;;) {
		/* try to increment readers/writer if block is compatible */
		st_change_prepare(bl, &old, &st);
		do {
			need_read = (st.mode < get_mode) || (!st.uptodate && !(nbf & NBF_NEW));
			acquired = !need_read && !conflicting_rw_refs(&st, nbf) &&
				   !((nbf & NBF_WRITE) && st.io_pending);

			if (acquired) {
				if (nbf & NBF_CONVERT_WRITE) {
					st.readers = 0;
					st.writer = 1;
				} else if (nbf & NBF_READ) {
					st.readers++;
				} else {
					st.writer = 1;
				}
			} else if (need_read && !st.io_pending) {
				st.io_pending = 1;
				st.read_mode = get_mode;
				st.read_no_data = (nbf & NBF_NEW) ? 1 : 0;
			}

		} while (!st_change_apply(bl, &old, &st));

		if (acquired)
			break;

		/* XXX could do this after submitting.. is it likely caller will retry? */
		if (nbf & (NBF_TRY | NBF_CONVERT_WRITE)) {
			ret = -EDEADLK;
			goto out;
		}

		if (need_read && !old.io_pending && st.io_pending) {
			get_block(bl); /* presence on submit lists */
			llist_add(&bl->submit_llnode, &blinf->submit.llist);
			try_queue_submit_work(blinf);
		}

		/* wait until io finishes and no conflicting users */
		wait_event(&bl->waitq, st_expr(bl, &st, !st.io_pending &&
							!conflicting_rw_refs(&st, nbf)));

		/* catch read io errors */
		if (st_expr(bl, &st, st.error)) {
			ret = bl->err;
			goto out;
		}
	}

	/* _NEW zeros block contents */
	if (nbf & NBF_NEW) {
		memset(ngnfs_block_buf(bl), 0, NGNFS_BLOCK_SIZE); /* XXX caller's job? */
		st_change(bl, &st, ((st.uptodate = 1), (st.error = 0)));
		bl->err = 0;
	}

	ret = 0;
out:
	if (ret < 0)  {
		put_block(bl);
		bl = ERR_PTR(ret);
	}

	return bl;
}

/*
 * Release a block reference.  The nbf flags must start with matching
 * the mode of the previously successful _get call but can add
 * additional flags that change behaviour.  (We might want a more robust
 * "holder" struct that flags then modify.)
 */
void ngnfs_block_put(struct ngnfs_fs_info *nfi, struct ngnfs_block *bl, nbf_t nbf)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block_state old;
	struct ngnfs_block_state st;
	bool dirtied = false;

	/* first set dirty while write ref gives us exclusive access to block */
	if ((nbf & NBF_WRITE) && !(nbf & NBF_NODIRTY) &&
	    st_cond_change(bl, &old, &st, !st.dirty, st.dirty = 1)) {
		get_block(bl);
		atomic_inc(&blinf->nr_dirty);
		/* XXX barrier? */
		llist_add(&bl->dirty_llnode, &blinf->flush.llist);
		dirtied = true;
	}

	/* then drop writer and let waiting other users at the block */
	st_change_prepare(bl, &old, &st);
	do {
		if (nbf & NBF_WRITE)
			st.writer = 0;
		else
			st.readers--;

	} while (!st_change_apply(bl, &old, &st));

	try_set_cache_mode(nfi, bl);
	wake_up(&bl->waitq);
	put_block(bl);

	if (dirtied)
		try_queue_flush_work(blinf);
}

void *ngnfs_block_buf(struct ngnfs_block *bl)
{
	return page_address(bl->page);
}

struct page *ngnfs_block_page(struct ngnfs_block *bl)
{
	return bl->page;
}

/*
 * Wait until the number of dirty blocks is under the hard limit.
 * There's no measures taken to avoid thundering herds, and once writers
 * proceed past this wait they can each dirty their full transaction's
 * worth of blocks.
 */
void ngnfs_block_dirty_limit_wait(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	/* XXX probably interruptible, io errors won't clear dirty */
	wait_event(&blinf->waitq, atomic_read(&blinf->nr_dirty) < DIRTY_LIMIT);
}

/*
 * This sync blocks until previously dirty blocks are flushed and
 * written out successfully.  It captures blocks that were dirtied by
 * _put calls that returned before this _sync call started.
 *
 * The error tracking is pretty clumsy.  The first write error seen
 * while any sync is waiting is returned to all waiting syncs.  It's
 * goofy, but easy.
 *
 * The ordering between waiters and dirty blocks is achieved by having
 * each waiter insert a fake block into the flush list.  It'll be woken
 * once the clean work finds it in the clean list after all previously
 * dirty blocks.
 */
int ngnfs_block_sync(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;
	struct ngnfs_block_state st;
	struct ngnfs_block *bl;

	if (atomic_read(&blinf->nr_dirty) == 0)
		return 0;

	bl = alloc_block(0, false);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	st_change(bl, &st, st.sync_waiter = 1);

	sync_waiters_inc(blinf);

	get_block(bl);
	llist_add(&bl->dirty_llnode, &blinf->flush.llist);
	try_queue_flush_work(blinf);

	wait_event(&bl->waitq, sync_waiters_has_error(blinf) || st_expr(bl, &st, !st.sync_waiter));
	put_block(bl);

	return sync_waiters_dec_error(blinf);
}

int ngnfs_block_setup(struct ngnfs_fs_info *nfi, int queue_depth)
{
	struct ngnfs_block_info *blinf;
	int ret;

	if (WARN_ON_ONCE(queue_depth < 1))
		return -EINVAL;

	blinf = kzalloc(sizeof(struct ngnfs_block_info), GFP_KERNEL);
	if (!blinf) {
		ret = -ENOMEM;
		goto out;
	}

	blinf->nfi = nfi;
	blinf->queue_depth = queue_depth;
	atomic_set(&blinf->nr_dirty, 0);
	atomic_set(&blinf->nr_flushing, 0);
	atomic_set(&blinf->nr_mode_flushing, 0);
	atomic_set(&blinf->nr_submitted, 0);
	atomic_set(&blinf->sync_waiters, 0);
	init_block_work_list(&blinf->flush, ngnfs_block_flush_work);
	init_block_work_list(&blinf->submit, ngnfs_block_submit_work);
	init_block_work_list(&blinf->clean, ngnfs_block_clean_work);
	init_waitqueue_head(&blinf->waitq);

	ret = rhashtable_init(&blinf->ht, &ngnfs_block_ht_params);
	if (ret < 0)
		goto out_free;

	/* XXX use fs identifier in name */
	blinf->flush.wq = create_singlethread_workqueue("ngnfs-flush");
	blinf->submit.wq = create_singlethread_workqueue("ngnfs-submit");
	blinf->clean.wq = create_singlethread_workqueue("ngnfs-clean");
	if (!blinf->flush.wq || !blinf->submit.wq || !blinf->clean.wq) {
		ret = -ENOMEM;
		goto out_destroy;
	}
	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_BLOCK_READ_RESULT, recv_block_read_result) ?:
	      ngnfs_msg_register_recv(nfi, NGNFS_MSG_BLOCK_WRITE_RESULT, recv_block_write_result) ?:
	      ngnfs_msg_register_recv(nfi, NGNFS_MSG_BLOCK_MODE_SET, recv_block_mode_set);
	if (ret < 0)
		goto out_destroy;

	nfi->block_info = blinf;
	ret = 0;
	goto out;

out_destroy:
	/* fine to call these if they aren't registered */
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_READ_RESULT, recv_block_read_result);
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_WRITE_RESULT, recv_block_write_result);
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_MODE_SET, recv_block_mode_set);
	destroy_block_work_list(&blinf->flush);
	destroy_block_work_list(&blinf->submit);
	destroy_block_work_list(&blinf->clean);
	rhashtable_destroy(&blinf->ht);
out_free:
	kfree(blinf);
out:
	return ret;
}

/*
 * The rhashtable caller is destroying the hash table as it calls us, we don't
 * have to remove blocks from the table.
 */
static void free_ht_block(void *ptr, void *arg)
{
	struct ngnfs_block *bl = ptr;

	put_block(bl);
}

/*
 * Once we're destroying we should have no more callers who would queue
 * work.  We shutdown the block transport to stop further IO completion
 * which could queue work.
 */
void ngnfs_block_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_block_info *blinf = nfi->block_info;

	if (blinf) {
		ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_READ_RESULT,
					  recv_block_read_result);
		ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_WRITE_RESULT,
					  recv_block_write_result);
		ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_BLOCK_MODE_SET,
					  recv_block_mode_set);

		/* any queued work is drained before destruction */
		destroy_block_work_list(&blinf->flush);
		destroy_block_work_list(&blinf->submit);
		destroy_block_work_list(&blinf->clean);

		rhashtable_free_and_destroy(&blinf->ht, free_ht_block, blinf);
		kfree(blinf);
		nfi->block_info = NULL;
	}
}
