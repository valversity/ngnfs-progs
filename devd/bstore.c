/* SPDX-License-Identifier: GPL-2.0 */

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/crc64.h"
#include "shared/lk/err.h"
#include "shared/lk/kernel.h"
#include "shared/lk/math.h"
#include "shared/lk/minmax.h"
#include "shared/lk/string.h"
#include "shared/lk/overflow.h"

#include "shared/bstore.h"
#include "shared/dtracef.h"
#include "shared/format-block.h"
#include "shared/format-dev.h"
#include "shared/hash_table.h"
#include "shared/string_wrappers.h"
#include "shared/summary_tree.h"

#include "utask/block.h"
#include "utask/utask.h"

#include "devd/bstore.h"

/*
 * This block store sits between devd request processing and the block
 * cache IO engine.  It manages safely reading stable blocks while new
 * dirty blocks are written, provides atomic updates to blocks and their
 * associated metadata, and indexes their metadata to accelerate
 * searches over the blocks.
 *
 * The device blocks are organized into four regions.  First are the
 * commit blocks that describe atomic changes.  Then comes the journal
 * as a staging ground for new versions of blocks to be written without
 * overwriting current blocks.  Next are two internal metadata block
 * regions used to describe stored blocks: summaries of block details
 * and the blocks full of details themselves.  Finally the bulk of the
 * device is used for storage of blocks that we send and receive over
 * the wire.
 *
 * A given block can be found at its final lba location or it can have
 * been written to the journal and not yet replayed out to its final
 * location.  We have a hash table that maps the logical block names to
 * their stable lba location.
 *
 * To write, blocks are dirtied in the block cache and are written
 * either to the journal if they were already in use or to their
 * physical lba.  Only if the writes succeed is the stable hash table
 * updated so readers will use the results of the write.
 *
 * The consistency model that governs access to the blocks between
 * readers and writers is built around the single threaded utask
 * runtime.  Readers always share references to the stable set of
 * blocks.  They never reference the dirty blocks.  Writers first act as
 * readers, getting shared references to any blocks they might need to
 * COW to write to the journal.  Their dirtying and modification of the
 * blocks is non-blocking so it is atomic and single threaded from the
 * perspective of the other tasks.  If readers sleep waiting for IO,
 * they check the lba mapping hash to see that the block hasn't changed
 * once they wake up.
 */

/*
 * A given commit will only be half full of foreground dirtied blocks.
 * The second half is used by journal replay to move blocks from the
 * oldest live commit out of the journal.
 */
#define MAX_PREPARED_ENTRIES (NGNFS_DEV_COMMIT_MAX_ENTRIES / 2)

static struct bstore_instance {
	struct hash_table *stable_ht;
	struct hash_table *dirty_ht;
	struct summary_tree *smt;
	struct ngnfs_uuid dev_uuid;
	bool have_uuid;
	struct ngnfs_dev_commit_block stable_cmt;

	/* convenience, set from commit layout at init */
	u64 commit_blocks;
	u64 journal_lba;
	u64 summary_lba;
	u64 details_lba;
	u64 storage_lba;
	u64 storage_blocks;

	struct utask *commit_tsk;
	struct utask_wait_queue commit_wq;
	struct ngnfs_dev_commit_block *dirty_cmt;
	struct cached_block *dirty_cmt_cblk;
	u64 commit_phase;
	int last_commit_ret;
	u64 non_replay_entries;

	struct utask *replay_tsk;
	struct utask_wait_queue replay_wq;
	/* static replay storage to keep it off the replay utask stack */
	struct {
		struct cached_block *cblk;
		unsigned int e;
	} replay_blocks[MAX_PREPARED_ENTRIES];

} global_bstore_inst = {
};

/*
 * All the static block addresses and block array indexes associated
 * with a given dev_bnr.
 */
struct dev_bnr_mapping {
	u64 lba;
	u64 details_lba;
	u64 summary_lba;
	unsigned int details_ind;
	unsigned int summary_ind;
};

static int map_dev_bnr(struct bstore_instance *inst, struct dev_bnr_mapping *map, u64 dev_bnr)
{
	u64 off;

	if (dev_bnr >= inst->storage_blocks)
		return -EINVAL;

	map->lba = inst->storage_lba + dev_bnr;

	map->details_lba = inst->details_lba + (dev_bnr / NGNFS_DEV_DETAILS_PER_BLOCK);
	map->details_ind = dev_bnr % NGNFS_DEV_DETAILS_PER_BLOCK;

	off = map->details_lba - inst->details_lba;
	map->summary_lba = inst->summary_lba + (off / NGNFS_DEV_SUMMARIES_PER_BLOCK);
	map->summary_ind = off % NGNFS_DEV_SUMMARIES_PER_BLOCK;

	return 0;
}

/*
 * This is used to get at the summary lba and ind for the details block.
 * We perform a normal mapping with the first dev_bnr recorded in the
 * details block.
 */
static void map_details_lba(struct bstore_instance *inst, struct dev_bnr_mapping *map,
			    u64 details_lba)
{
	u64 dev_bnr = (details_lba - inst->details_lba) * NGNFS_DEV_DETAILS_PER_BLOCK;
	int ret;

	ret = map_dev_bnr(inst, map, dev_bnr);
	BUG_ON(ret < 0); /* dirty commit should have valid lbas */
}

static u64 stable_commit_ctr(struct bstore_instance *inst)
{
	return le64_to_cpu(inst->stable_cmt.commit_ctr);
}

/*
 * Return the lba of the most recent stable version of a block if it's
 * found in the journal.  If the lba isn't in the hash table then the
 * real lba is returned.
 */
static u64 stable_lba(struct bstore_instance *inst, u64 lba)
{
	return htable_lookup(inst->stable_ht, lba) ?: lba;
}

/*
 * Returns a dirty lba > 0 if the lba is currently dirty and has an
 * entry in the commit.  Returns 0 if the lba hasn't yet been tracked as
 * dirty in the commit.  Can return the lba itself when the journaled
 * write is to the lba's final location.
 */
static u64 dirty_lba(struct bstore_instance *inst, u64 lba)
{
	return htable_lookup(inst->dirty_ht, lba);
}

static bool lba_in_journal(struct bstore_instance *inst, u64 lba)
{
	return lba >= inst->journal_lba && lba < inst->summary_lba;
}

static bool type_has_header(u8 type)
{
	return type != NGNFS_DEV_BLOCK_TYPE_STORED;
}

static u64 calc_block_crc(void *buf, bool has_header)
{
	size_t off = has_header ? sizeof_field(struct ngnfs_dev_block_header, crc) : 0;

	return crc64_nvme(0, buf + off, NGNFS_BLOCK_SIZE - off);
}

static void init_hdr(struct bstore_instance *inst, struct ngnfs_dev_block_header *hdr, u8 type)
{
	hdr->crc = 0;
	hdr->dev_uuid = inst->dev_uuid;
	memset_zero_sizeof(hdr->pad_);
	hdr->type = type;
}

struct verify_hdr_args {
	struct bstore_instance *inst;
	u8 type;
};

static bool init_zeroed_header(struct bstore_instance *inst, struct ngnfs_dev_block_header *hdr,
			       u8 type)
{
	if (type_has_header(type) && mem_is_zero(hdr, sizeof(struct ngnfs_dev_block_header))) {
		init_hdr(inst, hdr, type);
		return true;
	}

	return false;
}

static int verify_hdr(struct cached_block *cblk, void *arg)
{
	struct ngnfs_dev_block_header *hdr = block_data_buf(cblk);
	struct verify_hdr_args *vha = arg;
	struct bstore_instance *inst = vha->inst;
	u64 crc;
	int ret;

	if (init_zeroed_header(inst, hdr, vha->type))
		return 0;

	crc = calc_block_crc(hdr, true);

	/* only verify the uuid once we've read a block and read it */
	if ((vha->type != hdr->type || crc != le64_to_cpu(hdr->crc)) ||
	    (inst->have_uuid &&
	     memcmp(&hdr->dev_uuid, &inst->dev_uuid, sizeof(struct ngnfs_uuid)))) {
		dtracef("bstore_verify_hdr_failed",
			"exp type %u crc %016llx hdr type %u crc %016llx",
			vha->type, crc, hdr->type, le64_to_cpu(hdr->crc));
		ret = -EUCLEAN;
	} else {
		ret = 0;
	}

	return ret;
}

/*
 * Read an internal metadata block and verify its header.  The block
 * cache drops the block if validation fails and remembers when
 * validation succeeds.
 *
 * Verification considers zeroed blocks as uninitialized and initializes
 * their header and returns success.
 */
static int read_block_hdr(struct bstore_instance *inst, u64 lba, u8 type,
			  struct cached_block **cblk_ret)
{
	struct verify_hdr_args vha = {
		.inst = inst,
		.type = type,
	};

	return block_read_verify(lba, verify_hdr, &vha, cblk_ret);
}

/*
 * A simple little helper that issues read-ahead in batches.
 */
static void readahead_batch(u64 x, u32 n, u64 limit, u32 stride)
{
	u32 i;

	if ((x % n) == 0) {
		for (i = 0; i < n && (x + i < limit); i++)
			block_readahead((x + i) * stride);
	}
}

/*
 * The commit phase is initialized to 0, is even when a dirty commit is
 * being built, and is odd when a commit is being written.
 */
static u64 current_commit_phase(struct bstore_instance *inst)
{
	return inst->commit_phase;
}

/*
 * Wait until the commit that the caller sampled is done.  The phase is
 * odd while a commit is being written.  Setting the low (odd) bit in
 * the caller's phase ensures that we wait for the commit they sampled
 * to complete whether it was dirtying or being written.
 */
static int wait_until_commit_done(struct bstore_instance *inst, u64 phase)
{
	return utask_wait_event(&inst->commit_wq, current_commit_phase(inst) > (phase | 1));
}

static u64 journal_blocks(struct bstore_instance *inst)
{
	return inst->summary_lba - inst->journal_lba;
}

static u64 commit_ctr_lba(struct bstore_instance *inst, u64 ctr)
{
	return ctr % inst->commit_blocks;
}

static u64 journal_index_ctr_lba(struct bstore_instance *inst, u64 ctr)
{
	return inst->journal_lba + (ctr % journal_blocks(inst));
}

static int journal_used_pct(struct bstore_instance *inst)
{
	struct ngnfs_dev_commit_block *cmt = &inst->stable_cmt;
	int c = (le64_to_cpu(cmt->commit_ctr) - le64_to_cpu(cmt->oldest_commit_ctr) + 1) * 100 /
		inst->commit_blocks;
	int j = (le64_to_cpu(cmt->journal_head_ctr) - le64_to_cpu(cmt->journal_tail_ctr)) * 100 /
		journal_blocks(inst);

	return max(c, j);
}

/*
 * Prepare resources for the caller to add block_count dirty blocks to
 * the dirty commit without blocking.
 *
 * If there isn't a dirty commit then we allocate a new commit block.
 *
 * If there isn't room in the commit then whoever added the current
 * blocks will wake the commit task which will write the commit once all
 * runnable tasks are done.  We wait until the commit finishes and
 * there's room again.
 *
 * If this returns success then the caller must call
 * finish_dirty_commit() which will block waiting for the commit to be
 * written.
 *
 * If we had to wait for a dirty commit, and the stable commit changed
 * in the interim, then we return -EAGAIN and the caller should retry.
 */
static int prepare_dirty_commit(struct bstore_instance *inst, u64 stable_ctr,
			        struct list_head *pool, u16 block_count, bool is_replay,
				struct ngnfs_dev_commit_block **cmt_ret)
{
	struct ngnfs_dev_commit_block *stable = &inst->stable_cmt;
	struct ngnfs_dev_commit_block *cmt;
	u64 commit_ctr;
	u64 lba;
	int ret;

	if (WARN_ON_ONCE(block_count > MAX_PREPARED_ENTRIES)) {
		ret = -EINVAL;
		goto out;
	}

	/* can't modify commit being written, everyone waits for io to complete */
	ret = utask_wait_event(&inst->commit_wq, (current_commit_phase(inst) & 1) == 0);
	if (ret < 0)
		goto out;

	if (!is_replay) {
		/* kick off replay if the journal is getting full, waiting if it's too full */
		if (journal_used_pct(inst) > 80)
			utask_wake_task(inst->replay_tsk);
		ret = utask_wait_event(&inst->commit_wq, journal_used_pct(inst) < 95);
		if (ret < 0)
			goto out;

		/* all non-replay share prepare max */
		ret = utask_wait_event(&inst->commit_wq, (inst->non_replay_entries + block_count)
								<= MAX_PREPARED_ENTRIES);
		if (ret < 0)
			goto out;
	}

	/* verify inputs after possibly sleeping */
	if (stable_ctr != stable_commit_ctr(inst)) {
		ret = -EAGAIN;
		goto out;
	}

	ret = block_alloc_pool(pool, !inst->dirty_cmt + block_count);
	if (ret < 0)
		goto out;

	/* past point of no return */
	if (!is_replay)
		inst->non_replay_entries += block_count;

	cmt = inst->dirty_cmt;
	if (!cmt) {
		commit_ctr = le64_to_cpu(stable->commit_ctr) + 1;
		lba = commit_ctr_lba(inst, commit_ctr);

		ret = block_create_dirty(lba, pool, NULL, &inst->dirty_cmt_cblk);
		BUG_ON(ret < 0); /* pool should have prevented failure */

		cmt = block_data_buf(inst->dirty_cmt_cblk);
		inst->dirty_cmt = cmt;

		init_hdr(inst, &cmt->hdr, NGNFS_DEV_BLOCK_TYPE_COMMIT);
		cmt->layout = stable->layout;
		cmt->commit_ctr = cpu_to_le64(commit_ctr);
		cmt->oldest_commit_ctr = stable->oldest_commit_ctr;
		cmt->journal_head_ctr = stable->journal_head_ctr;
		cmt->journal_tail_ctr = stable->journal_tail_ctr;
		cmt->nr_entries = 0;
		cmt->nr_in_journal = 0;
		memset_zero_sizeof(cmt->pad_);
	}

	ret = 0;
out:
	if (ret < 0)
		cmt = NULL;
	*cmt_ret = cmt;
	return ret;
}

/*
 * The caller has sampled the stable commit_ctr and gathered their
 * inputs for dirtying blocks in a commit.  This returns true if they
 * should retry.  It returns false if they can proceed, and *ret < 0 if
 * there was an error.  If *ret == 0 then the pool and commit are
 * prepared for dirtying the commit and finish_dirty_commit() must be
 * called when they're done.
 */
static bool retry_prepare_dirty(struct bstore_instance *inst, u64 stable_ctr, int *ret,
				struct list_head *pool, u16 block_count, bool is_replay,
				struct ngnfs_dev_commit_block **cmt_ret)
{
	if (stable_ctr != stable_commit_ctr(inst))
		return true;

	if (*ret == 0) {
		*ret = prepare_dirty_commit(inst, stable_ctr, pool, block_count, is_replay,
					    cmt_ret);
		if (*ret == -EAGAIN) {
			*ret = 0;
			return true;
		}
	}

	return false;
}

/*
 * The caller has finished updating dirty blocks in the current dirty
 * commit.  This schedules the commit write task and waits for it to
 * finish.
 *
 * The first utask that starts to wait after dirtying will put the
 * commit task at the end of the run list.  Any other dirtying tasks on
 * the run list will have a chance to add their blocks to the commit,
 * but that's it.  They'll have a chance on the next commit when they're
 * woken after waiting for the commit to finish.  We might want to tune
 * that, but more likely we'd want to add support for more commits in
 * flight.
 */
static int finish_dirty_commit(struct bstore_instance *inst, struct list_head *pool)
{
	u64 phase = current_commit_phase(inst);
	int ret;

	block_free_pool(pool);
	utask_wake_task(inst->commit_tsk);

	ret = wait_until_commit_done(inst, phase);
	if (ret == 0)
	       ret = inst->last_commit_ret;

	return ret;
}

/*
 * Give the caller a reference to a dirty block that will be written in
 * the current dirty commit.  If the lba is already dirty in the commit
 * then we return a reference to that existing dirty block.
 *
 * We prefer to write to the real lba to avoid journal replay host write
 * amplification (at the cost of unfriendly write patterns for GC,
 * causing device write amplification).  We can only do this when the
 * real lba isn't currently in use -- that is, either it's current
 * stable version is in the journal or the real lba is unused.
 *
 * For now, the dirty block contents are copied from the original (or
 * are zeroed).  This could be further optimized, but it'd make block
 * naming and buffer sharing a bit more complicated.  It'd probably be
 * worth it.
 */
static void dirty_block(struct bstore_instance *inst, struct ngnfs_dev_commit_block *cmt,
		        struct list_head *pool, u64 lba, u8 type, bool lba_unused,
			struct page *data_page, struct cached_block *copy_from_cblk,
			struct cached_block **cblk)
{
	struct ngnfs_dev_commit_entry *ent;
	u64 dlba;
	u16 nr;
	int ret;

	/* only called on real lbas that would naturally be after the journal */
	BUG_ON(lba < inst->summary_lba);
	BUG_ON(lba >= inst->storage_lba + inst->storage_blocks);

	dlba = dirty_lba(inst, lba);
	if (dlba > 0) {
		ret = block_lookup(dlba, cblk);
		/* must be dirty and pinned in the block cache */
		BUG_ON(ret < 0);
		return;
	}

	if (lba_in_journal(inst, stable_lba(inst, lba)) || lba_unused) {
		dlba = lba;
	} else {
		dlba = journal_index_ctr_lba(inst, le64_to_cpu(cmt->journal_head_ctr));
		le64_add_cpu(&cmt->journal_head_ctr, 1);
		le16_add_cpu(&cmt->nr_in_journal, 1);
	}

	/* Callers must have prepared correct block count to ensure free entries */
	nr = le16_to_cpu(cmt->nr_entries);
	BUG_ON(nr >= NGNFS_DEV_COMMIT_MAX_ENTRIES);
	cmt->nr_entries = cpu_to_le16(nr + 1);
	ent = &cmt->entries[nr];

	ent->lba = cpu_to_le64(lba);
	ent->journ_lba = cpu_to_le64(dlba);
	ent->crc = 0;
	memset_zero_sizeof(ent->pad_);
	ent->type = type;

	ret = block_create_dirty(dlba, pool, data_page, cblk);
	BUG_ON(ret < 0); /* prepare should have preallocated */

	htable_insert(inst->dirty_ht, lba, dlba);

	if (copy_from_cblk)
		memcpy(block_data_buf(*cblk), block_data_buf(copy_from_cblk), NGNFS_BLOCK_SIZE);
	else if (!data_page)
		memset(block_data_buf(*cblk), 0, NGNFS_BLOCK_SIZE);

	init_zeroed_header(inst, block_data_buf(*cblk), type);

	dtracef("bstore_dirty_block", "lba %llu type %u journ_lba %llu", lba, type, dlba);
}

/*
 * A convenience for callers who are working with blocks they know must
 * be pinned and dirty, asserting if they're not.
 */
static void already_dirty_block(struct bstore_instance *inst, u64 lba, struct cached_block **cblk)
{
	u64 dlba;
	int ret;

	/* only commit blocks can have an lba of 0, this is called for others */
	dlba = dirty_lba(inst, lba);
	BUG_ON(dlba == 0);

	ret = block_lookup(dlba, cblk);
	BUG_ON(ret < 0);
}

/*
 * We accelerate searching for stored blocks by maintaining a tree of
 * summaries of their block details.  The lowest levels are in
 * persistent blocks because they're large enough to not reasonably fit
 * in memory.  Above those we maintain a tree in memory.
 */
static u64 summarize_smt_words(u64 *words, unsigned short nr)
{
	u64 summary = 0;
	int i;

	for (i = 0; i < nr; i++)
		summary = max(summary, words[i]);

	return summary;
}

static u64 summarize_summary_block(struct ngnfs_dev_summary_block *sblk)
{
	u64 summary = 0;
	int i;

	for (i = 0; i < NGNFS_DEV_SUMMARIES_PER_BLOCK; i++)
		summary = max(summary, le64_to_cpu(sblk->summaries[i]));

	return summary;
}

static u64 summarize_details_block(struct ngnfs_dev_details_block *dblk)
{
	u64 summary = 0;
	int i;

	for (i = 0; i < NGNFS_DEV_DETAILS_PER_BLOCK; i++)
		summary = max(summary, le64_to_cpu(dblk->details[i].write_ctr));

	return summary;
}

/*
 * Update the in-memory tracking of the summary of a summary block.
 */
static int update_summary_block(struct bstore_instance *inst, u64 lba)
{
	struct ngnfs_dev_summary_block *sblk;
	struct cached_block *cblk;
	u64 summary;
	u64 pos;
	int ret;

	ret = read_block_hdr(inst, stable_lba(inst, lba), NGNFS_DEV_BLOCK_TYPE_SUMMARY, &cblk);
	if (ret < 0)
		goto out;

	sblk = block_data_buf(cblk);
	summary = summarize_summary_block(sblk);
	block_put(cblk);

	pos = lba - inst->summary_lba;
	smtree_set(inst->smt, summarize_smt_words, pos, summary);
	ret = 0;
out:
	return ret;
}

/*
 * Update the stable hash table's mapping of lbas to their stable
 * location in the journal.
 */
static void update_commit_lbas(struct bstore_instance *inst, struct ngnfs_dev_commit_block *cmt)
{
	struct ngnfs_dev_commit_entry *ent;
	int i;

	/* update the htable so readers get either real lba or journaled location */
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		dtracef("bstore_update_stable_lba",
			"lba %llu journ_lba %llu",
			le64_to_cpu(ent->lba), le64_to_cpu(ent->journ_lba));

		if (ent->lba == ent->journ_lba)
			htable_delete(inst->stable_ht, le64_to_cpu(ent->lba));
		else
			htable_insert(inst->stable_ht, le64_to_cpu(ent->lba),
						       le64_to_cpu(ent->journ_lba));
	}
}

/*
 * Update the summary tracking of all the summary blocks that changed in
 * a commit.  This is only run as commits are successfully written.
 */
static int update_commit_summaries(struct bstore_instance *inst,
				   struct ngnfs_dev_commit_block *cmt)
{
	struct ngnfs_dev_commit_entry *ent;
	int ret;
	int i;

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		if (ent->type == NGNFS_DEV_BLOCK_TYPE_SUMMARY) {
			ret = update_summary_block(inst, le64_to_cpu(ent->lba));
			if (ret < 0)
				goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

/*
 * The current dirty commit modified the given details block.  Update
 * its word in its summaries block.
 */
static void update_dirty_summary(struct bstore_instance *inst, u64 det_lba)
{
	struct ngnfs_dev_details_block *dblk;
	struct ngnfs_dev_summary_block *sblk;
	struct cached_block *det_cblk;
	struct cached_block *sum_cblk;
	struct dev_bnr_mapping map;

	map_details_lba(inst, &map, det_lba);

	already_dirty_block(inst, det_lba, &det_cblk);
	already_dirty_block(inst, map.summary_lba, &sum_cblk);

	dblk = block_data_buf(det_cblk);
	sblk = block_data_buf(sum_cblk);

	sblk->summaries[map.summary_ind] = cpu_to_le64(summarize_details_block(dblk));

	block_put(det_cblk);
	block_put(sum_cblk);
}

/*
 * Write out the current dirty commit.
 *
 * This is woken by tasks once they've successfully dirtied blocks in
 * the current commit.  When it's woken its put on the run list after
 * all other runnable tasks so they have a chance to add to the dirty
 * commit if they don't sleep.
 *
 * Once this run it doesn't block until it has updated the commit phase
 * which stops other tasks from dirtying until the write IOs finish.
 *
 * This finalizes the dirty blocks now that they won't be dirtied again.
 * We calculate their crcs and the summaries of modified detail blocks.
 */
static void write_dirty_commit(struct bstore_instance *inst, struct ngnfs_dev_commit_block *cmt)
{
	struct ngnfs_dev_commit_entry *ent;
	struct ngnfs_dev_block_header *hdr;
	struct cached_block *cblk = NULL;
	bool has_header;
	u64 crc;
	int ret;
	int i;

	dtracef("bstore_commit_prepare",
		"phase %llu ctr %llu old_ctr %llu head_ctr %llu tail_ctr %llu ents %u in_journ %u",
		inst->commit_phase, le64_to_cpu(cmt->commit_ctr),
		le64_to_cpu(cmt->oldest_commit_ctr), le64_to_cpu(cmt->journal_head_ctr),
		le64_to_cpu(cmt->journal_tail_ctr), le16_to_cpu(cmt->nr_entries),
		le16_to_cpu(cmt->nr_in_journal));

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		if (ent->type == NGNFS_DEV_BLOCK_TYPE_DETAILS)
			update_dirty_summary(inst, le64_to_cpu(ent->lba));
	}

	/* finalize the crcs on all the blocks in the commit */
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];

		ret = block_lookup(le64_to_cpu(ent->journ_lba), &cblk);
		BUG_ON(ret); /* must be pinned and dirty */

		has_header = type_has_header(ent->type);

		crc = calc_block_crc(block_data_buf(cblk), has_header);
		ent->crc = cpu_to_le64(crc);

		if (has_header) {
			hdr = block_data_buf(cblk);
			hdr->crc = cpu_to_le64(crc);
		}

		block_putp(&cblk);
	}

	crc = calc_block_crc(cmt, true);
	cmt->hdr.crc = cpu_to_le64(crc);

	inst->commit_phase++;

	ret = block_write_all_dirty();
	if (ret == 0) {
		update_commit_lbas(inst, cmt);
		ret = update_commit_summaries(inst, cmt);
		BUG_ON(ret < 0); /* reading pinned dirty blocks shouldn't fail */
		inst->stable_cmt = *cmt;
	}

	htable_clear(inst->dirty_ht);
	block_clean_all_dirty();
	inst->dirty_cmt = NULL;
	block_putp(&inst->dirty_cmt_cblk);

	inst->non_replay_entries = 0;
	inst->last_commit_ret = ret;
	inst->commit_phase++;
	utask_wake_all(&inst->commit_wq);
}

/*
 * Woken by finishing block dirtying or by shutdown.  When shutting down
 * there may not be a dirty commit.
 */
static void commit_write_utask(void *data)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct ngnfs_dev_commit_block *cmt;

	do {
		utask_wait_event_task((cmt = inst->dirty_cmt));
		if (cmt)
			write_dirty_commit(inst, cmt);

	} while (!utask_am_canceled());
}


/*
 * We attempt to replay blocks in the oldest commit whenever the journal
 * gets too full.
 *
 * The only blocks it has to replay are those for whom the most recent
 * version of the block is found in the oldest commit, but first it has
 * to read them to be able to write them somewhere else.  While the
 * oldest commit can not change, the blocks we're trying to read can be
 * updated by more recent commits.
 *
 * Once we've possible slept reading all the blocks we thought we'd have
 * to replay, we double check the version and location of the blocks.
 * New versions might have been written, or the block might be dirty in
 * the current dirty commit.
 *
 * Normal foreground writers limit themselves to the half of the entries
 * in a dirty commit.  This ensures that we'll always have the other
 * half of the entries for recording replayed blocks.  Since we only
 * have to replay blocks in the journal, we can always write them out to
 * their real location.
 *
 * The replay task is not guaranteed to be the last task that dirties
 * blocks in the commit.  Later tasks can see the dirty replay blocks
 * and modify them.  If we could ensure that replay was the last user of
 * the commit, then we could ensure that replayed blocks are never
 * modified, and we could skip them when calculating crcs or updating
 * summaries.
 */
static void replay_oldest_commit(struct bstore_instance *inst)
{
	struct ngnfs_dev_commit_block *dirty_cmt;
	struct ngnfs_dev_commit_block *cmt;
	struct ngnfs_dev_commit_entry *ent;
	struct cached_block *dirty_cblk;
	struct cached_block *cblk = NULL;
	LIST_HEAD(pool);
	u64 stable_ctr;
	u64 journ_lba;
	u64 lba;
	u16 replay_nr = 0;
	u16 i;
	int ret;

	stable_ctr = le64_to_cpu(inst->stable_cmt.commit_ctr);
	lba = commit_ctr_lba(inst, le64_to_cpu(inst->stable_cmt.oldest_commit_ctr));
	ret = read_block_hdr(inst, lba, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		goto out;

	cmt = block_data_buf(cblk);
	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];
		lba = le64_to_cpu(ent->lba);
		journ_lba = le64_to_cpu(ent->journ_lba);

		/* will naturally skip replayed blocks in the commit */
		if (!lba_in_journal(inst, journ_lba) || stable_lba(inst, lba) != journ_lba)
			continue;

		inst->replay_blocks[replay_nr].e = i;
		ret = block_read(journ_lba, &inst->replay_blocks[replay_nr].cblk);
		if (ret < 0)
			goto out;

		replay_nr++;
	}

	ret = prepare_dirty_commit(inst, stable_ctr, &pool, replay_nr, true, &dirty_cmt);
	if (ret < 0)
		goto out;

	for (i = 0; i < replay_nr; i++) {
		ent = &cmt->entries[inst->replay_blocks[i].e];
		lba = le64_to_cpu(ent->lba);
		journ_lba = le64_to_cpu(ent->journ_lba);

		/* check that block is still current and not dirty in this commit */
		if (stable_lba(inst, lba) != journ_lba || dirty_lba(inst, lba) != 0)
			continue;

		dirty_block(inst, dirty_cmt, &pool, lba, ent->type, true, NULL,
			    inst->replay_blocks[i].cblk, &dirty_cblk);
		block_put(dirty_cblk);
	}

	le64_add_cpu(&dirty_cmt->oldest_commit_ctr, 1);
	le64_add_cpu(&dirty_cmt->journal_tail_ctr, le16_to_cpu(dirty_cmt->nr_in_journal));

	ret = finish_dirty_commit(inst, &pool);
out:
	/* might as well zero to tidy up */
	for (i = 0; i < replay_nr; i++) {
		block_putp(&inst->replay_blocks[i].cblk);
		inst->replay_blocks[i].e = 0;
	}

	block_put(cblk);
}

/*
 * Only add dirty replay blocks when there older stable commits and we
 * haven't already replayed in the current dirty commit.
 */
static bool should_replay(struct bstore_instance *inst)
{
	return inst->stable_cmt.oldest_commit_ctr != inst->stable_cmt.commit_ctr &&
	       (!inst->dirty_cmt || inst->dirty_cmt->oldest_commit_ctr ==
				    inst->stable_cmt.oldest_commit_ctr);
}

/*
 * Woken by finishing block dirtying or by shutdown.  When shutting down
 * there may not be a dirty commit.
 */
static void journal_replay_utask(void *data)
{
	struct bstore_instance *inst = &global_bstore_inst;
	int ret;

	do {
		ret = utask_wait_event_task(should_replay(inst));
		if (ret == 0)
			replay_oldest_commit(inst);

	} while (!utask_am_canceled());
}

/*
 * Give the caller a reference to the current version of the block that
 * stores the given dev_bnr.
 *
 * The network protocol doesn't yet make use of the block details.  We
 * do read the details block to reflect the IO cost of eventually doing
 * so.
 *
 * The caller's block reference is also escaping the protection of
 * checking the stable commit.  That's OK because today they copy the
 * block into a send buffer before blocking so the block won't be
 * modified.
 */
int bstore_read(u64 dev_bnr, struct cached_block **cblk)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct cached_block *det_cblk = NULL;
	struct dev_bnr_mapping map;
	u64 ctr;
	int ret;

	ret = map_dev_bnr(inst, &map, dev_bnr);
	if (ret < 0)
		goto out;

	do {
		ctr = stable_commit_ctr(inst);

		block_putp(&det_cblk);
		block_putp(cblk);

		block_readahead(stable_lba(inst, map.lba));
		ret = read_block_hdr(inst, stable_lba(inst, map.details_lba),
				     NGNFS_DEV_BLOCK_TYPE_DETAILS, &det_cblk) ?:
		      block_read(stable_lba(inst, map.lba), cblk);

	} while (stable_commit_ctr(inst) != ctr);

out:
	block_putp(&det_cblk);

	dtracef("bstore_read", "dev_bnr %llu ret %d", dev_bnr, ret);
	return ret;
}

int bstore_write(u64 dev_bnr, struct page *data_page)
{
	struct bstore_instance *inst = &global_bstore_inst;
	struct ngnfs_dev_details_block *dblk;
	struct ngnfs_dev_commit_block *cmt;
	struct ngnfs_block_details *stable_det = NULL;
	struct ngnfs_block_details *in_det = NULL;
	struct ngnfs_block_details *det;
	struct cached_block *det_cblk = NULL;
	struct cached_block *sum_cblk = NULL;
	struct cached_block *cblk = NULL;
	struct dev_bnr_mapping map;
	LIST_HEAD(pool);
	u64 nr;
	int ret;

	ret = map_dev_bnr(inst, &map, dev_bnr);
	if (ret < 0)
		goto out;

	do {
		nr = stable_commit_ctr(inst);

		block_putp(&det_cblk);
		block_putp(&sum_cblk);

		block_readahead(stable_lba(inst, map.summary_lba));
		ret = read_block_hdr(inst, stable_lba(inst, map.details_lba),
				     NGNFS_DEV_BLOCK_TYPE_DETAILS, &det_cblk) ?:
		      read_block_hdr(inst, stable_lba(inst, map.summary_lba),
				     NGNFS_DEV_BLOCK_TYPE_SUMMARY, &sum_cblk);

	} while (retry_prepare_dirty(inst, nr, &ret, &pool, 3, false, &cmt));
	if (ret < 0)
		goto out;

	/* faking incoming details, don't yet have protocol support */
	dblk = block_data_buf(det_cblk);
	stable_det = &dblk->details[map.details_ind];
	in_det = stable_det;

	/* update the dirty block details for the write */
	dirty_block(inst, cmt, &pool, map.details_lba, NGNFS_DEV_BLOCK_TYPE_DETAILS, false,
		    NULL, det_cblk, &cblk);
	dblk = block_data_buf(cblk);
	det = &dblk->details[map.details_ind];
	*det = *in_det;
	le64_add_cpu(&det->write_ctr, 1);
	/* don't have alloc/free */
	if (det->lifetime_ctr == 0)
		le64_add_cpu(&det->lifetime_ctr, 1);
	block_putp(&cblk);

	/* make sure we have a dirty summary block for write-time updates */
	dirty_block(inst, cmt, &pool, map.summary_lba, NGNFS_DEV_BLOCK_TYPE_SUMMARY, false,
		    NULL, sum_cblk, &cblk);
	block_putp(&cblk);

	/* and dirty the stored block with a reference to the data page */
	dirty_block(inst, cmt, &pool, map.lba, NGNFS_DEV_BLOCK_TYPE_STORED,
		    !(le64_to_cpu(stable_det->lifetime_ctr) & 1), data_page, NULL, &cblk);
	block_putp(&cblk);

	ret = finish_dirty_commit(inst, &pool);
out:
	block_free_pool(&pool);
	block_putp(&det_cblk);
	block_putp(&sum_cblk);

	dtracef("bstore_write", "dev_bnr %llu ret %d", dev_bnr, ret);
	return ret;
}

/*
 * Verify that all the blocks in a commit were successfully written.  We
 * read the commit block, readahead all the blocks in entries, and then
 * check their crcs.
 */
static int check_complete_commit(struct bstore_instance *inst, u64 commit_ctr)
{
	struct ngnfs_dev_commit_block *cmt;
	struct ngnfs_dev_commit_entry *ent;
	struct cached_block *cmt_cblk = NULL;
	struct cached_block *cblk = NULL;
	u64 journ_lba;
	u64 lba;
	u64 crc;
	int ret;
	int i;

	lba = commit_ctr_lba(inst, commit_ctr);
	ret = read_block_hdr(inst, lba, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cmt_cblk);
	if (ret < 0)
		goto out;
	cmt = block_data_buf(cmt_cblk);

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];
		journ_lba = le64_to_cpu(ent->journ_lba);

		block_readahead(journ_lba);
	}

	for (i = 0; i < le16_to_cpu(cmt->nr_entries); i++) {
		ent = &cmt->entries[i];
		journ_lba = le64_to_cpu(ent->journ_lba);

		ret = block_read(journ_lba, &cblk);
		if (ret < 0)
			goto out;

		crc = calc_block_crc(block_data_buf(cblk), type_has_header(ent->type));
		block_put(cblk);

		if (le64_to_cpu(ent->crc) != crc) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = 0;
out:
	block_put(cmt_cblk);

	dtracef("bstore_check_complete_commit", "ctr %llu ret %d", commit_ctr, ret);
	return ret;
}

/*
 * Find the most recent complete commit that was written.  We first
 * perform a binary search of commit blocks to find the greatest
 * commit_ctr.  We then make sure all its blocks were written, and check
 * the previous commit if they weren't.
 *
 * The binary search is a little tricky to picture because we're probing
 * for the small contiguous range of ctrs that can be present in the
 * ring.  We implement the search range in terms of the full precision
 * counters.  Each read maps to a commit block lba, and then we can
 * clamp the search range given the ctr we find and the adjacent ctrs
 * that could be possible in the ring of blocks.
 */
static int find_stable_commit(struct bstore_instance *inst, u64 *commit_ctr_ret)
{
	struct ngnfs_dev_commit_block *cmt;
	struct cached_block *cblk = NULL;
	u64 greatest = 0;
	u64 start = 0;
	u64 end = U64_MAX;
	u64 last;
	u64 mid;
	u64 ctr;
	u64 lba;
	int ret;

	while (start <= end) {
		mid = start + ((end - start) >> 1);
		lba = commit_ctr_lba(inst, mid);

		block_putp(&cblk);
		ret = block_read(lba, &cblk);
		if (ret < 0)
			goto out;

		/* XXX not verifying this */
		cmt = block_data_buf(cblk);

		dtracef("bstore_find_stable",
			"start %llu mid %llu end %llu gr %llu lba %llu cmt type %u ctr %llu",
			start, mid, end, greatest, lba, cmt->hdr.type,
			le64_to_cpu(cmt->commit_ctr));

		/* so far only trimming at format, 0s are unwritten tail */
		if (cmt->hdr.type == NGNFS_DEV_BLOCK_TYPE_UNINIT) {
			if (lba == 0) {
				/* format should have written to first commit block */
				ret = -EINVAL;
				goto out;
			}
			end = lba - 1;
			continue;
		}

		/* record greatest, continuing search for greater */
		ctr = le64_to_cpu(cmt->commit_ctr);
		if (ctr > greatest) {
			greatest = ctr;
			if (ctr == U64_MAX)
				break;
			start = ctr + 1;

		} else if (ctr >= start) {
			start = ctr + 1;
		}

		/* limit to last possible assuming we read smallest */
		last = ctr + min(U64_MAX - ctr, inst->commit_blocks - 1);
		if (last < end)
			end = last;
	}

	ret = check_complete_commit(inst, greatest);
	if (ret < 0 && greatest > 0)
		ret = check_complete_commit(inst, --greatest);

out:
	*commit_ctr_ret = greatest;
	block_put(cblk);

	dtracef("bstore_find_stable_commit", "ctr %llu ret %d", greatest, ret);
	return ret;
}

/*
 * Init the inst with the device layout.  We get the layout from the
 * first few commits that must have been written by formatting the
 * device.
 */
static int init_journal(struct bstore_instance *inst)
{
	struct ngnfs_dev_commit_block *cmt;
	struct cached_block *cblk = NULL;
	u64 journal_blocks;
	u64 summary_blocks;
	u64 details_blocks;
	u64 total;
	int ret;

	/* no trimming yet so we can always read the first few commit blocks */
	ret = read_block_hdr(inst, 0, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		ret = read_block_hdr(inst, 1, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		goto out;

	cmt = block_data_buf(cblk);

	/* catch initialized zero blocks :/ */
	if (cmt->layout.storage_blocks == 0) {
		ret = -EINVAL;
		goto out;
	}

	inst->dev_uuid = cmt->hdr.dev_uuid;
	inst->have_uuid = true;
	inst->commit_blocks = le64_to_cpu(cmt->layout.commit_blocks);
	journal_blocks = le64_to_cpu(cmt->layout.journal_blocks);
	summary_blocks = le64_to_cpu(cmt->layout.summary_blocks);
	details_blocks = le64_to_cpu(cmt->layout.details_blocks);
	inst->storage_blocks = le64_to_cpu(cmt->layout.storage_blocks);

	total = 0;
	if (check_add_overflow(inst->commit_blocks, journal_blocks, &total) ||
	    check_add_overflow(summary_blocks, total, &total) ||
	    check_add_overflow(details_blocks, total, &total) ||
	    check_add_overflow(inst->storage_blocks, total, &total)) {
		printf("%s: overflow in calculating total blocks\n", __func__);
		bstore_print_commit_block(cmt);
		ret = -EUCLEAN;
		goto out;
	}

	if (total > block_total_blocks()) {
		printf("%s: error in block accounting: total blocks %llu > device total %llu\n",
		       __func__, total, block_total_blocks());
		bstore_print_commit_block(cmt);
		ret = -EUCLEAN;
		goto out;
	}

	ret = bstore_check(cmt);
	if (ret < 0)
		goto out;

	inst->stable_ht = htable_alloc(journal_blocks);
	if (!inst->stable_ht) {
		ret = -ENOMEM;
		goto out;
	}

	inst->smt = smtree_alloc(8, summary_blocks);
	if (!inst->smt) {
		ret = -ENOMEM;
		goto out;
	}

	inst->journal_lba = inst->commit_blocks;
	inst->summary_lba = inst->journal_lba + journal_blocks;
	inst->details_lba = inst->summary_lba + summary_blocks;
	inst->storage_lba = inst->details_lba + details_blocks;

	ret = 0;
out:
	block_put(cblk);
	return ret;
}

/*
 * Load all the live commits into memory.  This is mostly updating the
 * stable hash table for the final location of blocks in journal.  But
 * we also save the header of the most recent commit.
 */
static int load_commit_blocks(struct bstore_instance *inst, u64 commit_ctr)
{
	struct ngnfs_dev_commit_block *cmt;
	struct cached_block *cblk;
	u64 lba;
	u64 nr;
	int ret;

	lba = commit_ctr_lba(inst, commit_ctr);
	ret = read_block_hdr(inst, lba, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
	if (ret < 0)
		goto out;

	cmt = block_data_buf(cblk);
	inst->stable_cmt = *cmt;
	nr = le64_to_cpu(cmt->oldest_commit_ctr);
	block_put(cblk);

	dtracef("bstore_load_commit_blocks", "old_ctr %llu ctr %llu", nr, commit_ctr);

	for (; nr <= commit_ctr; nr++) {
		lba = commit_ctr_lba(inst, nr);

		readahead_batch(lba, 16, inst->journal_lba, 1);

		ret = read_block_hdr(inst, lba, NGNFS_DEV_BLOCK_TYPE_COMMIT, &cblk);
		if (ret < 0)
			goto out;

		cmt = block_data_buf(cblk);
		update_commit_lbas(inst, cmt);
		block_put(cblk);
	}

	ret = 0;
out:
	return ret;
}

static int load_summary_blocks(struct bstore_instance *inst)
{
	u64 lba;
	int ret = 0;

	for (lba = inst->summary_lba; lba < inst->details_lba; lba++) {
		readahead_batch(lba, 16, inst->details_lba, 1);

		ret = update_summary_block(inst, lba);
		if (ret < 0)
			break;
	}

	return ret;
}

int bstore_init(void)
{
	struct bstore_instance *inst = &global_bstore_inst;
	u64 commit_ctr;
	int ret;

	utask_init_wait_queue(&inst->commit_wq);
	utask_init_wait_queue(&inst->replay_wq);

	inst->dirty_ht = htable_alloc(NGNFS_DEV_COMMIT_MAX_ENTRIES);
	if (!inst->dirty_ht) {
		ret = -ENOMEM;
		goto out;
	}

	ret = init_journal(inst) ?:
	      find_stable_commit(inst, &commit_ctr) ?:
	      load_commit_blocks(inst, commit_ctr) ?:
	      load_summary_blocks(inst) ?:
	      utask_create_nowake(commit_write_utask, inst, &inst->commit_tsk) ?:
	      utask_create_nowake(journal_replay_utask, inst, &inst->replay_tsk);
out:
	if (ret < 0)
		bstore_exit();
	return ret;
}

void bstore_exit(void)
{
	struct bstore_instance *inst = &global_bstore_inst;

	utask_destroy(inst->replay_tsk);
	utask_destroy(inst->commit_tsk);
	free(inst->stable_ht);
	free(inst->dirty_ht);
	memset(inst, 0, sizeof(struct bstore_instance));
}
