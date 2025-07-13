/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "shared/lk/align.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/compiler_attributes.h"
#include "shared/lk/list.h"

#include "shared/dtracef.h"
#include "shared/valgrind_support.h"

#include "utask/utask.h"

/*
 * This utask layer offers voluntary pre-emption of call stacks in
 * userspace.  The interface is modeled after the Linux kernel's task
 * switching interfaces.  I suppose the "u task" name evokes both
 * "userspace" or micro ("µ") tasks.
 *
 * Unlike the kernel, the motivation for this is specifically to avoid
 * concurrency.  We want to write easy to understand sequential
 * procedural code paths that block.  But we limit them to one execution
 * thread.  At any moment, only one utask can be executing.  This lets
 * us build much simpler data structures which only have to consider
 * re-entrance and not concurrency, and especially not concurrent
 * mutation.
 *
 * These aren't coroutines.  We're not emitting output between
 * cooperating functions by returning back and forth between them.
 * We're constraining the problem to specifically jumping from a
 * scheduling thread context, to a utask's stack, and back as it blocks
 * or returns.
 *
 * utasks consist of an aligned allocated stack and a utask struct.  The
 * utask struct is stored at the base of the stack to reduce
 * allocations, but that might be sufficiently unwelcome stack pressure
 * to motivate us to allocate it separately.
 *
 * Eventually all utasks block and some external input wakes them to
 * introduce forward progress.  This is where io_uring comes in.  The
 * utask core maintains an active ring.  All tasks can prepare io_uring
 * commands while they're executingg, and utask provides some helpers to
 * give submissions a hook to get a callback on completion.  The
 * scheduler then blocks by entering uring to process any prepared
 * submissions.  It waits for completions and calls the registered
 * callbacks, which presumably wake utasks.
 */

static struct utask_instance {
	struct list_head run_list;
	struct list_head tsk_list;
	struct io_uring ring;
	u64 next_id;
	bool ring_initialized;
	bool shutdown;
	struct utask_cqe_callback shutdown_nop_cb;

} global_utask_inst = {
	.run_list = LIST_HEAD_INIT(global_utask_inst.run_list),
	.tsk_list = LIST_HEAD_INIT(global_utask_inst.tsk_list),
	.ring_initialized = false,
};

struct utask {
	unsigned long magic;
	struct list_head run_head;
	struct list_head tsk_head;
	struct list_head wait_head;
	unsigned long canceled:1,
		      finished:1;
	char *name;
	u64 id;
	VGS_DEFINE_STACK_ID(vg_stack_id);
	struct utask *destroyer;
	const char *sched_file;
	const char *sched_func;
	unsigned int sched_line;
	utask_fn_t fn;
	void *data;
	void *stack;
	void *sched_sp;
	void *sp;
};

/*
 * Return the utask pointer for the executing utask by masking the stack
 * pointer.  Returns NULL if we're not executing in a utask.
 *
 * We're assuming that we can always dereference the magic found at in
 * the end of the aligned region of the stack as though it was a full
 * utask's size.  This might be too risky :(.
 */
static struct utask *get_current_utask(void)
{
	struct utask *tsk;
	unsigned long sp;

	asm("movq %%rsp, %0 		\n\t"
	    : "=rm" (sp) : : );

	tsk = (void *)(round_up(sp, UTASK_STACK_SIZE) - sizeof(struct utask));

	return tsk->magic == UTASK_MAGIC ? tsk : NULL;
}

/*
 * Return the pointer to the utask that we're executing in.  This
 * asserts if it's not called from within a utask.
 */
struct utask *utask_current(void)
{
	struct utask *tsk = get_current_utask();

	BUG_ON(!tsk);
	return tsk;
}

/*
 * Returns the task ID of the current task.  IDs are assigned
 * sequentially from 1 and are never re-used.  Returns 0 when this isn't
 * run in a utask.
 */
u64 utask_current_id(void)
{
	struct utask *tsk = get_current_utask();

	return tsk ? tsk->id : 0;
}

void utask_init_wait_queue(struct utask_wait_queue *wq)
{
	INIT_LIST_HEAD(&wq->wait_list);
}

bool utask_waitqueue_active(struct utask_wait_queue *wq)
{
	return !list_empty(&wq->wait_list);
}

void utask_prepare_wait(struct utask_wait_queue *wq, struct utask_wait_entry *wait)
{
	list_add_tail(&wait->queue_head, &wq->wait_list);
	wait->tsk = utask_current();
}

void utask_finish_wait(struct utask_wait_entry *wait)
{
	if (!list_empty(&wait->queue_head))
		list_del_init(&wait->queue_head);
}

void utask_wake_all(struct utask_wait_queue *wq)
{
	struct utask_wait_entry *wait;

	list_for_each_entry(wait, &wq->wait_list, queue_head)
		utask_wake_task(wait->tsk);
}

/*
 * Add the utask to the scheduler's run list.  it will be switched to
 * and executed after the rest of utasks currently on the run list.
 * This can be called from a utask executing within a scheduler's
 * current _run call.
 */
void utask_wake_task(struct utask *tsk)
{
	struct utask_instance *inst = &global_utask_inst;

	if (!tsk->finished && list_empty(&tsk->run_head))
		list_add_tail(&tsk->run_head, &inst->run_list);
}

/*
 * Mark a task as canceled and wake it up.  _wait_event calls made by
 * the task will will return with -ECANCELED if the condition isn't met
 * and _am_canceled() will return true.
 *
 * This is a nop for a null task so that it can be used in teardown
 * paths without conditionals.
 */
void utask_cancel(struct utask *tsk)
{
	if (tsk && !tsk->canceled) {
		tsk->canceled = 1;
		utask_wake_task(tsk);
	}
}

/*
 * Returns true if the current task has been marked canceled.
 */
bool utask_am_canceled(void)
{
	struct utask *tsk = utask_current();

	return tsk->canceled;
}

/*
 * Free the given tsk once it finished execution by returning from its
 * fn ("finished").  If the task has already finished then this can be
 * called from outside of utasks.
 *
 * If the task hasn't finished, then the marked canceled to cause it to
 * return from wait event calls.  This must be called from a different
 * utask so we can wait for the given task to finish.  Only one caller
 * can wait on and destroy a given task.
 *
 * This should be called for every successfully created task during
 * clean shutdown.
 *
 * This is a nop for a null task so that it can be used in teardown
 * paths without conditionals.
 */
void utask_destroy(struct utask *tsk)
{
	if (tsk) {
		dtracef("utask_destroy", "name %c%c%c()",
			tsk->name[0], tsk->name[1], tsk->name[2]);

		if (!tsk->finished) {
			BUG_ON(tsk == utask_current());
			BUG_ON(tsk->destroyer);

			utask_cancel(tsk);
			tsk->destroyer = utask_current();

			utask_wait_event_nocancel(tsk->finished);
		}

		if (!list_empty(&tsk->run_head))
			list_del_init(&tsk->run_head);
		if (!list_empty(&tsk->wait_head))
			list_del_init(&tsk->wait_head);

		VGS_STACK_DEREGISTER(tsk->vg_stack_id);
		free(tsk->stack);
	}
}

/*
 * Destroys the specified utask unless it is the current executing
 * utask.  This is useful when a utask is exiting and tearing down an
 * object that it shares with other utasks.  It will be destroyed as it
 * returns, so it only needs to destroy the other utasks associated with
 * the object.
 */
void utask_destroy_other(struct utask *tsk)
{
	if (tsk != utask_current())
		utask_destroy(tsk);
}

/*
 * Switch to and execute all the utasks on the run list.  Each will
 * either schedule or finish and return.  The run_list can safely be
 * modified by the utasks while they're running.
 */
int utask_run(void)
{
	struct utask_instance *inst = &global_utask_inst;
	struct utask_cqe_callback *cb;
	struct io_uring_cqe *cqe;
	struct utask *tsk;
	int ret;

	for (;;) {
		/* execute all runnable tasks */
		while ((tsk = list_first_entry_or_null(&inst->run_list, struct utask, run_head))) {
			list_del_init(&tsk->run_head);

			dtracef("utask_run", "switching to %c%c%c()",
				tsk->name[0], tsk->name[1], tsk->name[2]);
			ret = utask_switch_to(tsk);
			if (ret == UTASK_RET_FINISHED) {
				tsk->finished = 1;
				if (tsk->destroyer)
					utask_wake_task(tsk->destroyer);
			}
		}

		if (inst->shutdown)
			break;

		/* submit and sqes that utasks prepared, and gather cqes */
		io_uring_submit_and_get_events(&inst->ring);

		/* XXX no timeouts yet */
		ret = io_uring_wait_cqe(&inst->ring, &cqe);
		BUG_ON(ret != 0);

		/* call completion function for each cqe */
		do {
			cb = io_uring_cqe_get_data(cqe);
			cb->fn(cqe, cb);

			io_uring_cqe_seen(&inst->ring, cqe);
		} while (io_uring_peek_cqe(&inst->ring, &cqe) == 0);
	}

	return 0;
}

static void shutdown_cb(struct io_uring_cqe *cqe, struct utask_cqe_callback *cb)
{
	struct utask_instance *inst = &global_utask_inst;

	inst->shutdown = 1;
}

/*
 * Start an orderly shutdown of utasks.
 *
 * utask_run() will return after it processes all the completion events
 * and runnable tasks that were ready after it processes the callback
 * for our nop which sets shutdown on the instance.
 *
 * This is a bit fragile and incomplete. It could ensure that there
 * aren't any more tasks other than the one calling _shutdown.
 */
void utask_shutdown(void)
{
	struct utask_instance *inst = &global_utask_inst;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(utask_ring());
	BUG_ON(!sqe);

	utask_set_sqe_callback(sqe, &inst->shutdown_nop_cb, shutdown_cb);
	io_uring_prep_nop(sqe);
}

/*
 * This _yield has the specific behaviour that it will schedule and let
 * any existing tasks in the run list execute before this yield call
 * returns.  It is a nop of nothing is in the run list.  If tasks that
 * we yield to themselves wake tasks, then those woken tasks will
 * execute *after* this yield call returns.  This only considers the run
 * list as it existed before the call.
 */
/* neat, but no callers have needed it? */
__unused
static void utask_yield(void)
{
	struct utask_instance *inst = &global_utask_inst;
	struct utask *tsk = utask_current();

	/* nothing in the run list */
	if (list_empty(&inst->run_list))
		return;

	/* only us in the run list */
	if (inst->run_list.next == inst->run_list.prev && inst->run_list.next == &tsk->run_head)
		return;

	/* make sure we're last on the run list before scheduling */
	if (list_empty(&tsk->run_head))
		list_add_tail(&tsk->run_head, &inst->run_list);
	else
		list_move_tail(&tsk->run_head, &inst->run_list);

	utask_schedule();
}

/*
 * Stop the calling task and switch from its stack to the scheduler's
 * after recording info about the time it spent running.
 */
void utask_schedule_info(const char *func, const char *file, unsigned int line)
{
	struct utask *tsk = utask_current();

	tsk->sched_func = func;
	tsk->sched_file = file;
	tsk->sched_line = line;

	utask_switch_from();
}

/*
 * Newly created utasks jump to this at the base of their stack
 */
static void utask_entry(void)
{
	struct utask *tsk = utask_current();

	tsk->fn(tsk->data);
	utask_finish();
}

/*
 * XXX very lp64 x86_64 specific.
 */
static void push_stack(struct utask *tsk, void *ptr)
{
	tsk->sp -= sizeof(ptr);
	*(void **)tsk->sp = ptr;
}

/*
 * Allocate and run a new utask.  The new task will be idle and it's up
 * to the caller to wake it.
 */
int utask_create_name_nowake(char *name, utask_fn_t fn, void *data, struct utask **tsk_ret)
{
	struct utask_instance *inst = &global_utask_inst;
	struct utask *tsk = NULL;
	unsigned long after;
	void *stack;
	int ret;

	/*
	 * dtracef doesn't support strings, so we print the first 3
	 * chars of the task name.
	 */
	BUG_ON(strlen(name) < 3);

	dtracef("utask_create", "task %llu name %c%c%c",
		inst->next_id, name[0], name[1], name[2]);

	ret = posix_memalign(&stack, UTASK_STACK_SIZE, UTASK_STACK_SIZE);
	if (ret != 0) {
		ret = -ret;
		goto out;
	}

	/* XXX assumes stack grows down */
	tsk = stack + UTASK_STACK_SIZE - sizeof(struct utask);

	tsk->magic = UTASK_MAGIC;
	INIT_LIST_HEAD(&tsk->run_head);
	INIT_LIST_HEAD(&tsk->wait_head);
	list_add_tail(&tsk->tsk_head, &inst->tsk_list);
	tsk->canceled = 0;
	tsk->finished = 0;
	tsk->name = name;
	tsk->id = inst->next_id++;
	VGS_STACK_REGISTER(tsk->vg_stack_id, (unsigned long)stack, (unsigned long)tsk - 1);
	tsk->destroyer = NULL;
	tsk->fn = fn;
	tsk->data = data;
	tsk->stack = stack;
	tsk->sp = tsk;

	/*
	 * Make sure sp is 16 byte aligned after we push the entry ret
	 * addr and initial zeroed regs.
	 */
	after = (unsigned long)tsk->sp - (2 * sizeof(void *)) - UTASK_SAVED_BYTES;
	if (!IS_ALIGNED(after, 16))
		tsk->sp -= 16 - (after & 15);

	/* final null address to terminate backtracing unwinders */
	push_stack(tsk, NULL);
	/* first switch_to jumps to _entry */
	push_stack(tsk, utask_entry);

	/* initial zeroed saved registers */
	tsk->sp -= UTASK_SAVED_BYTES;
	memset(tsk->sp, 0, UTASK_SAVED_BYTES);

	ret = 0;
out:
	if (ret < 0)
		tsk = NULL;
	*tsk_ret = tsk;
	return ret;
}

/*
 * Allocate and run a new utask.  If this returns success then the fn
 * will be called from the newly created utask.
 */
int utask_create_name(char *name, utask_fn_t fn, void *data, struct utask **tsk_ret)
{
	int ret = utask_create_name_nowake(name, fn, data, tsk_ret);
	if (ret == 0)
		utask_wake_task(*tsk_ret);
	return ret;
}

/*
 * The utask runtime has one global io_uring that subsystems prepare
 * into.  Each sets user_data to a callback that's called as cqes are
 * received.
 */
struct io_uring *utask_ring(void)
{
	struct utask_instance *inst = &global_utask_inst;

	return &inst->ring;
}

/*
 * Set the sqe's user_data so that we can use it when parsing cqes to
 * call the submission's completion function.
 */
void utask_set_sqe_callback(struct io_uring_sqe *sqe, struct utask_cqe_callback *cb,
			    utask_cqe_fn_t fn)
{
	cb->fn = fn;
	io_uring_sqe_set_data(sqe, cb);
}

static void wake_waiter_cqe_fn(struct io_uring_cqe *cqe, struct utask_cqe_callback *cb)
{
	struct utask_cqe_waiter *wait = container_of(cb, struct utask_cqe_waiter, cb);

	wait->res = cqe->res;
	wait->completed = true;
	utask_wake_task(wait->tsk);
}

/*
 * Calling utasks can get an sqe that they prepare and then schedule
 * waiting for a completion of.  This leaves a reference in the sqe/cqe
 * user_data to the utask's stack.  This can be safe if used in utasks
 * that cancel submitted commands before destroy utasks and freeing
 * their stacks.
 *
 * io_uring_prep_* must be called on the sqe that is returned.  Then the
 * caller can wait for waiter->completed.
 */
struct io_uring_sqe *utask_get_sqe_waiter(struct utask_cqe_waiter *waiter)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(utask_ring());
	BUG_ON(!sqe); /* not sure how to throttle/wake callers */

	waiter->completed = false;
	waiter->tsk = utask_current();
	utask_set_sqe_callback(sqe, &waiter->cb, wake_waiter_cqe_fn);

	return sqe;
}

void utask_print_tasks(void)
{
	struct utask_instance *inst = &global_utask_inst;
	u64 cid = utask_current_id();
	struct utask *tsk;

	fprintf(stderr, "\n");
	list_for_each_entry(tsk, &inst->tsk_list, tsk_head) {
		fprintf(stderr, "  %llu %c%c%c %s ",
			tsk->id,
			(tsk->canceled ? 'C' : '-'),
			(tsk->finished ? 'F' : '-'),
			(tsk->id == cid ? 'R' : '-'),
			tsk->name);

		if (tsk->id != cid && tsk->sched_func)
			fprintf(stderr, "slept in %s() at %s:%u\n",
				tsk->sched_func, tsk->sched_file, tsk->sched_line);
		else
			fprintf(stderr, "\n");
	}
}

int utask_init(u32 entries)
{
	struct utask_instance *inst = &global_utask_inst;
	int ret;

	inst->next_id = 1;

	ret = io_uring_queue_init(entries, &inst->ring, IORING_SETUP_COOP_TASKRUN |
				  IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);
	if (ret > 0)
		inst->ring_initialized = true;

	return ret;
}

void utask_exit(void)
{
	struct utask_instance *inst = &global_utask_inst;

	if (inst->ring_initialized) {
		io_uring_queue_exit(&inst->ring);
		inst->ring_initialized = false;
	}

	inst->shutdown = false;
}

/*
 * This hack emits defines in the compiled asm output so that they can
 * be extracted into a header file and included by assembly source.
 *
 * The goofy extern declaration silences sparse, making it static stops
 * it from being compiled.
 */
extern void generate_compiled_defines(void);
void generate_compiled_defines(void)
{
#define DEFINE_VALUE(SYMBOL, VALUE) \
    __asm__  ("#define " #SYMBOL " %a0" :: "n"(VALUE))

	DEFINE_VALUE(UTASK_ASM_SIZEOF_UTASK, sizeof(struct utask));
	DEFINE_VALUE(UTASK_ASM_OFFSETOF_MAGIC, offsetof(struct utask, magic));
	DEFINE_VALUE(UTASK_ASM_OFFSETOF_SCHED_SP, offsetof(struct utask, sched_sp));
	DEFINE_VALUE(UTASK_ASM_OFFSETOF_SP, offsetof(struct utask, sp));
}
