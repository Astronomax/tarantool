#include "index_build.h"

#include "key_list.h"
#include "schema.h"
#include "space.h"
#include "trigger.h"
#include "tuple.h"
#include "trivia/util.h"
#include "txn.h"
#include "txn_limbo.h"
#include "journal.h"
#include "errinj.h"
#include "box.h"
#include "say.h"

#define RB_COMPACT 1
#include "small/rb.h"
#include "small/region.h"

struct index_build_ctx;

/**
 * A write point in the building index. The write set is shared between
 * the main build loop and concurrent transactions: the main loop adds a
 * point for every tuple it reads into the current batch, a concurrent
 * transaction adds a point for every tuple it inserts. When a
 * transaction prepares a key, all other in-progress transactions
 * writing the same key are aborted.
 */
struct index_build_write_point {
	struct tuple *tuple;
	/**
	 * For multikey indexes: the index of the element within the array
	 * that this write point represents. HINT_NONE for non-multikey.
	 */
	hint_t hint;
	struct txn *txn;
	struct index_build_ctx *ctx;
	rb_node(struct index_build_write_point) in_ctx;
};

typedef rb_tree(struct index_build_write_point) index_build_write_set_t;

/**
 * Per-statement triggers attached to a concurrent transaction's
 * statement by the build on_replace trigger.
 */
struct index_build_stmt_trigger {
	struct rlist in_ctx;
	bool has_run_on_prepare;
	struct trigger on_prepare;
	struct trigger on_commit;
	struct trigger on_rollback;
	struct index_build_ctx *ctx;
};

/**
 * A tuple collected by the main loop for the current batch.
 */
struct index_build_batch_tuple {
	struct rlist in_ctx;
	struct rlist in_inserted;
	struct tuple *tuple;
};

/**
 * State shared between the batch transaction and its commit/rollback
 * triggers: the build context, the list of all batch tuples (for
 * write set cleanup) and the list of tuples actually inserted into the
 * building index (for commit/rollback).
 */
struct index_build_batch_state {
	struct index_build_ctx *ctx;
	struct rlist batch;
	struct rlist inserted;
};

/**
 * Ongoing index build state.
 */
struct index_build_ctx {
	struct index *index;
	struct tuple_format *format;
	bool check_unique_constraint;
	const struct index_build_vtab *vtab;
	/** Primary key definition, used to compare tuples with the cursor. */
	struct key_def *cmp_def;
	/**
	 * Saved cursor: the last tuple of the last committed batch
	 * (NULL at start). The next batch starts iterating from here.
	 */
	struct tuple *cursor;
	/**
	 * Current cursor: the last tuple read by the in-progress batch
	 * (NULL while no batch is running). Used by the concurrent
	 * on_prepare trigger to tell whether a key has already been
	 * passed by the current batch.
	 */
	struct tuple *batch_cursor;
	/** The in-progress batch transaction (NULL between batches). */
	struct txn *build_txn;
	/** Set to restart the in-progress batch from the saved cursor. */
	bool restart_batch;
	index_build_write_set_t write_set;
	struct rlist stmt_triggers;
	bool is_failed;
	struct diag diag;
	struct trigger on_replace;
	/** Source space id and the new index id, for _index_local. */
	uint32_t space_id;
	uint32_t iid;
};

static inline int
index_build_write_set_cmp(const struct index_build_write_point *a,
			  const struct index_build_write_point *b)
{
	struct key_def *key_def = a->ctx->index->def->key_def;
	int rc = tuple_compare(a->tuple, a->hint,
			       b->tuple, b->hint, key_def);
	if (rc == 0)
		rc = a->txn < b->txn ? -1 : a->txn > b->txn;
	return rc;
}

rb_gen(MAYBE_UNUSED static inline, index_build_write_set_,
       index_build_write_set_t, struct index_build_write_point,
       in_ctx, index_build_write_set_cmp);

static int
index_build_on_replace(struct trigger *trigger, void *event);

static int
index_build_on_prepare_stmt(struct trigger *base, void *event);

static int
index_build_on_commit_stmt(struct trigger *base, void *event);

static int
index_build_on_rollback_stmt(struct trigger *base, void *event);

static void
index_build_write_set_free(struct index_build_ctx *ctx);

int
generic_index_build_insert_main(struct index *index, struct tuple *tuple)
{
	(void)index;
	(void)tuple;
	return 0;
}

int
generic_index_build_commit_main(struct index *index, struct tuple *tuple)
{
	(void)index;
	(void)tuple;
	return 0;
}

int
generic_index_build_rollback_main(struct index *index, struct tuple *tuple)
{
	(void)index;
	(void)tuple;
	return 0;
}

int
generic_index_build_replace_concurrent(struct index *index,
				       struct tuple *old_tuple,
				       struct tuple *new_tuple)
{
	(void)index;
	(void)old_tuple;
	(void)new_tuple;
	return 0;
}

int
generic_index_build_rollback_concurrent(struct index *index,
					struct tuple *old_tuple,
					struct tuple *new_tuple)
{
	(void)index;
	(void)old_tuple;
	(void)new_tuple;
	return 0;
}

int
generic_index_build_iter_stmt(struct txn_stmt *stmt,
			      index_build_stmt_iter_f cb, void *arg)
{
	return cb(stmt->old_tuple, stmt->new_tuple, arg);
}

static struct index_build_write_point *
index_build_write_point_new(struct tuple *tuple, hint_t hint, struct txn *txn,
			    struct index_build_ctx *ctx)
{
	/*
	 * Write set entries outlive any single batch transaction (the set
	 * spans the whole build and concurrent transactions), so they can't
	 * live on a transaction region. Allocate them on the heap.
	 */
	struct index_build_write_point *point =
		xmalloc(sizeof(struct index_build_write_point));
	point->tuple = tuple;
	tuple_ref(point->tuple);
	point->hint = hint;
	/*
	 * For functional indexes the hint is a heap-allocated key tuple
	 * (struct tuple *) cast to hint_t. Keep it alive for the lifetime
	 * of this write point.
	 */
	if (ctx->index->def->key_def->for_func_index)
		tuple_ref((struct tuple *)hint);
	point->txn = txn;
	point->ctx = ctx;
	return point;
}

static void
index_build_write_point_delete(struct index_build_write_point *point)
{
	tuple_unref(point->tuple);
	if (point->ctx->index->def->key_def->for_func_index)
		tuple_unref((struct tuple *)point->hint);
	free(point);
}

static void
index_build_track_one(struct tuple *tuple, hint_t hint, struct txn *txn,
		      struct index_build_ctx *ctx)
{
	struct index_build_write_point *new_point =
		index_build_write_point_new(tuple, hint, txn, ctx);
	if (index_build_write_set_search(&ctx->write_set, new_point) != NULL) {
		index_build_write_point_delete(new_point);
		return;
	}
	index_build_write_set_insert(&ctx->write_set, new_point);
}

/**
 * Add @a tuple of transaction @a txn to the write set.
 * Returns -1 on allocation error (functional index key allocation), 0 on
 * success.
 */
static int
index_build_track(struct tuple *tuple, struct txn *txn,
		  struct index_build_ctx *ctx)
{
	if (tuple == NULL)
		return 0;
	struct key_def *key_def = ctx->index->def->key_def;
	if (key_def->for_func_index) {
		struct region *region = &fiber()->gc;
		size_t region_svp = region_used(region);
		struct key_list_iterator it;
		if (key_list_iterator_create(&it, tuple, ctx->index->def, true,
					     tuple_format_runtime) != 0) {
			region_truncate(region, region_svp);
			return -1;
		}
		struct tuple *key_tuple = NULL;
		int rc = 0;
		while ((rc = key_list_iterator_next(&it, &key_tuple)) == 0 &&
		       key_tuple != NULL) {
			/*
			 * key_list_iterator returns region-allocated tuples;
			 * make a heap copy so it survives region truncation.
			 * write_point_new refs the heap copy.
			 */
			uint32_t key_size;
			const char *key_data =
				tuple_data_range(key_tuple, &key_size);
			struct tuple *heap_key =
				tuple_new(tuple_format_runtime,
					  key_data, key_data + key_size);
			if (heap_key == NULL) {
				rc = -1;
				break;
			}
			index_build_track_one(tuple, (hint_t)heap_key, txn, ctx);
			key_tuple = NULL;
		}
		region_truncate(region, region_svp);
		return rc;
	}
	if (key_def->is_multikey) {
		int count = tuple_multikey_count(tuple, key_def);
		for (int i = 0; i < count; i++)
			index_build_track_one(tuple, (hint_t)i, txn, ctx);
	} else {
		index_build_track_one(tuple, HINT_NONE, txn, ctx);
	}
	return 0;
}

static void
index_build_untrack_one(struct tuple *tuple, hint_t hint, struct txn *txn,
			struct index_build_ctx *ctx)
{
	struct index_build_write_point key = {
		.tuple = tuple,
		.hint = hint,
		.txn = txn,
		.ctx = ctx,
	};
	struct index_build_write_point *point =
		index_build_write_set_search(&ctx->write_set, &key);
	if (point == NULL)
		return;
	index_build_write_set_remove(&ctx->write_set, point);
	index_build_write_point_delete(point);
}

/** Remove the write set entries for @a tuple of transaction @a txn. */
static void
index_build_untrack(struct tuple *tuple, struct txn *txn,
		    struct index_build_ctx *ctx)
{
	if (tuple == NULL)
		return;
	struct key_def *key_def = ctx->index->def->key_def;
	if (key_def->for_func_index) {
		/*
		 * Re-generate keys for comparison only. Region-allocated
		 * key tuples are used as hints to find the stored entries;
		 * the stored entries hold their own heap refs.
		 */
		struct region *region = &fiber()->gc;
		size_t region_svp = region_used(region);
		struct key_list_iterator it;
		if (key_list_iterator_create(&it, tuple, ctx->index->def, false,
					     tuple_format_runtime) != 0) {
			region_truncate(region, region_svp);
			ctx->is_failed = true;
			diag_move(diag_get(), &ctx->diag);
			return;
		}
		struct tuple *key_tuple = NULL;
		while (key_list_iterator_next(&it, &key_tuple) == 0 &&
		       key_tuple != NULL) {
			index_build_untrack_one(tuple, (hint_t)key_tuple,
						txn, ctx);
			key_tuple = NULL;
		}
		region_truncate(region, region_svp);
		return;
	}
	if (key_def->is_multikey) {
		int count = tuple_multikey_count(tuple, key_def);
		for (int i = 0; i < count; i++)
			index_build_untrack_one(tuple, (hint_t)i, txn, ctx);
	} else {
		index_build_untrack_one(tuple, HINT_NONE, txn, ctx);
	}
}

static void
index_build_abort_writers_one(struct tuple *tuple, hint_t hint,
			      struct txn *cur_txn,
			      struct index_build_ctx *ctx)
{
	struct index_build_write_point key = {
		.tuple = tuple,
		.hint = hint,
		.txn = NULL,
		.ctx = ctx,
	};
	struct key_def *key_def = ctx->index->def->key_def;

	struct index_build_write_set_iterator it;
	index_build_write_set_isearch_ge(&ctx->write_set, &key, &it);
	for (struct index_build_write_point *abort =
	     index_build_write_set_iterator_get(&it);
	     abort != NULL &&
	     tuple_compare(tuple, hint,
			   abort->tuple, abort->hint, key_def) == 0;
	     abort = index_build_write_set_inext(&it)) {
		if (abort->txn == cur_txn)
			continue;
		assert(abort->txn != NULL);
		txn_abort_with_conflict(abort->txn);
	}
}

/**
 * Abort every in-progress transaction that has a write point for any SK key
 * of @a tuple in the write set, except the current one.
 */
static void
index_build_abort_writers(struct tuple *tuple, struct txn *cur_txn,
			   struct index_build_ctx *ctx)
{
	if (tuple == NULL)
		return;
	struct key_def *key_def = ctx->index->def->key_def;
	if (key_def->for_func_index) {
		struct region *region = &fiber()->gc;
		size_t region_svp = region_used(region);
		struct key_list_iterator it;
		if (key_list_iterator_create(&it, tuple, ctx->index->def, false,
					     tuple_format_runtime) != 0) {
			region_truncate(region, region_svp);
			ctx->is_failed = true;
			diag_move(diag_get(), &ctx->diag);
			return;
		}
		struct tuple *key_tuple = NULL;
		while (key_list_iterator_next(&it, &key_tuple) == 0 &&
		       key_tuple != NULL) {
			index_build_abort_writers_one(tuple, (hint_t)key_tuple,
						      cur_txn, ctx);
			key_tuple = NULL;
		}
		region_truncate(region, region_svp);
		return;
	}
	if (key_def->is_multikey) {
		int count = tuple_multikey_count(tuple, key_def);
		for (int i = 0; i < count; i++)
			index_build_abort_writers_one(tuple, (hint_t)i,
						      cur_txn, ctx);
	} else {
		index_build_abort_writers_one(tuple, HINT_NONE, cur_txn, ctx);
	}
}

static struct index_build_write_point *
index_build_write_set_free_cb(index_build_write_set_t *write_set,
			      struct index_build_write_point *point,
			      void *arg)
{
	(void)arg;
	(void)write_set;
	index_build_write_point_delete(point);
	return NULL;
}

static void
index_build_write_set_free(struct index_build_ctx *ctx)
{
	index_build_write_set_iter(
		&ctx->write_set, NULL, index_build_write_set_free_cb, NULL);
}

/** Return true if the current transaction is aborted. */
static inline bool
index_build_txn_is_aborted(void)
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return false;
	return txn->status == TXN_ABORTED ||
	       txn_has_flag(txn, TXN_STMT_ROLLBACK);
}

/**
 * Check whether @a new_tuple conflicts with a tuple already present in
 * the building index. The lookup goes through the engine-specific
 * get_prepared vtab method, which only sees tuples that have already
 * been prepared.
 */
static int
index_build_check_is_unique_secondary_one(struct tuple *old_tuple,
					  struct tuple *new_tuple,
					  struct index *index,
					  int multikey_idx,
					  bool is_key,
					  struct index_build_ctx *ctx)
{
	assert(new_tuple != NULL);

	struct key_def *key_def = index->def->key_def;
	if (key_def->is_nullable &&
	    tuple_key_contains_null(new_tuple, key_def, multikey_idx))
		return 0;

	size_t region_svp = region_used(&fiber()->gc);
	const char *key;
	uint32_t part_count = key_def->part_count;
	if (is_key) {
		key = tuple_data(new_tuple);
		mp_decode_array(&key);
	} else {
		key = tuple_extract_key(new_tuple, key_def, multikey_idx, NULL);
		if (key == NULL)
			return -1;
		mp_decode_array(&key);
	}

	struct tuple *dup_tuple = ctx->vtab->get_prepared(index, key,
							  part_count);
	region_truncate(&fiber()->gc, region_svp);

	if (index_build_txn_is_aborted())
		return 0;

	if (dup_tuple != NULL && old_tuple != dup_tuple) {
		diag_set(ClientError, ER_TUPLE_FOUND,
			 index->def->name,
			 index->def->space_name,
			 tuple_str(dup_tuple),
			 tuple_str(new_tuple),
			 dup_tuple, new_tuple);
		return -1;
	}
	return 0;
}

static int
index_build_check_is_unique_secondary(struct tuple *old_tuple,
				      struct tuple *new_tuple,
				      struct index *index,
				      struct index_build_ctx *ctx)
{
	struct key_def *key_def = index->def->key_def;
	if (key_def->for_func_index) {
		assert(new_tuple != NULL);
		struct region *region = &fiber()->gc;
		size_t region_svp = region_used(region);
		struct key_list_iterator it;
		if (key_list_iterator_create(&it, new_tuple, index->def,
					     true,
					     tuple_format_runtime) != 0) {
			region_truncate(region, region_svp);
			return -1;
		}
		int rc = 0;
		struct tuple *key_tuple = NULL;
		while ((rc = key_list_iterator_next(&it, &key_tuple)) == 0 &&
		       key_tuple != NULL) {
			rc = index_build_check_is_unique_secondary_one(
				old_tuple, key_tuple, index, MULTIKEY_NONE,
				true, ctx);
			key_tuple = NULL;
			if (rc != 0)
				break;
			if (index_build_txn_is_aborted())
				return 0;
		}
		region_truncate(region, region_svp);
		return rc;
	}
	if (!index->def->key_def->is_multikey) {
		return index_build_check_is_unique_secondary_one(old_tuple,
								 new_tuple,
								 index,
								 MULTIKEY_NONE,
								 false, ctx);
	}
	int count = tuple_multikey_count(new_tuple, index->def->key_def);
	for (int i = 0; i < count; ++i) {
		if (index_build_check_is_unique_secondary_one(old_tuple,
							      new_tuple,
							      index, i,
							      false, ctx) != 0)
			return -1;
	}
	return 0;
}

static void
index_build_stmt_trigger_add(struct txn_stmt *stmt, struct index_build_ctx *ctx)
{
	struct index_build_stmt_trigger *stmt_trigger =
		xregion_alloc_object(&in_txn()->region,
				     struct index_build_stmt_trigger);
	stmt_trigger->has_run_on_prepare = false;
	stmt_trigger->ctx = ctx;
	rlist_add_entry(&ctx->stmt_triggers, stmt_trigger, in_ctx);
	trigger_create(&stmt_trigger->on_prepare,
		       index_build_on_prepare_stmt, NULL, NULL);
	trigger_create(&stmt_trigger->on_commit,
		       index_build_on_commit_stmt, NULL, NULL);
	trigger_create(&stmt_trigger->on_rollback,
		       index_build_on_rollback_stmt, NULL, NULL);
	txn_stmt_on_prepare(stmt, &stmt_trigger->on_prepare);
	txn_stmt_on_commit(stmt, &stmt_trigger->on_commit);
	txn_stmt_on_rollback(stmt, &stmt_trigger->on_rollback);
}

static void
index_build_stmt_trigger_delete(struct index_build_stmt_trigger *trigger)
{
	rlist_del_entry(trigger, in_ctx);
}

/**
 * on_replace trigger installed on the source space for the duration of
 * the build. Validates the new tuple, checks the unique constraint
 * (aborting this transaction, not the build, on a duplicate), registers
 * the statement in the write set and attaches per-statement
 * prepare/commit/rollback triggers.
 */
struct index_build_on_replace_arg {
	struct txn *txn;
	struct index_build_ctx *ctx;
	bool has_pair;
	bool txn_aborted;
};

static int
index_build_on_replace_f(struct tuple *old_tuple, struct tuple *new_tuple,
			  void *data)
{
	struct index_build_on_replace_arg *a = data;
	if (a->txn_aborted || (old_tuple == NULL && new_tuple == NULL))
		return 0;
	a->has_pair = true;
	if (new_tuple != NULL &&
	    tuple_validate(a->ctx->format, new_tuple) != 0)
		return -1;
	if (a->ctx->check_unique_constraint && new_tuple != NULL) {
		if (index_build_check_is_unique_secondary(old_tuple, new_tuple,
							  a->ctx->index,
							  a->ctx) != 0)
			return -1;
		if (index_build_txn_is_aborted()) {
			a->txn_aborted = true;
			return 0;
		}
		if (index_build_track(new_tuple, a->txn, a->ctx) != 0)
			return -1;
	}
	return 0;
}

static int
index_build_on_replace(struct trigger *trigger, void *event)
{
	assert(!index_build_txn_is_aborted());

	struct txn *txn = event;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	struct index_build_ctx *ctx = trigger->data;

	struct index_build_on_replace_arg arg = {
		.txn = txn, .ctx = ctx, .has_pair = false, .txn_aborted = false,
	};
	if (ctx->vtab->iter_stmt(stmt, index_build_on_replace_f, &arg) != 0)
		return -1;
	if (!arg.has_pair || arg.txn_aborted)
		return 0;
	index_build_stmt_trigger_add(stmt, ctx);
	return 0;
}

/**
 * on_prepare trigger of a concurrent transaction's statement.
 *
 * The statement is about to become prepared. Depending on where its
 * primary key is relative to the build cursor:
 *  - already passed by the main loop (left of the saved cursor): apply
 *    the change to the building index now, since the main loop won't
 *    visit this key again;
 *  - passed by the current, still uncommitted batch (between the saved
 *    and the current cursor): the batch must be restarted so that it
 *    re-reads this tuple - MVCC aborts the batch automatically, and we
 *    signal a restart for the non-MVCC fallback;
 *  - not yet reached (right of the current cursor): do nothing, the
 *    main loop will read and insert this tuple itself.
 *
 * Then abort all in-progress transactions conflicting with the new key
 * (which may abort the build batch and thus the whole build) and drop
 * this transaction's write point.
 */
struct index_build_on_prepare_arg {
	struct index_build_ctx *ctx;
	int csw;
	bool done;
	bool any_between;
};

static int
index_build_on_prepare_f(struct tuple *old_tuple, struct tuple *new_tuple,
			  void *data)
{
	struct index_build_on_prepare_arg *a = data;
	if (a->done)
		return 0;
	struct index_build_ctx *ctx = a->ctx;
	struct tuple *mark = new_tuple != NULL ? new_tuple : old_tuple;
	if (mark == NULL)
		return 0;

	bool le = ctx->cursor != NULL &&
		  tuple_compare(mark, HINT_NONE, ctx->cursor, HINT_NONE,
				ctx->cmp_def) <= 0;
	bool between = !le && ctx->batch_cursor != NULL &&
		       tuple_compare(mark, HINT_NONE, ctx->batch_cursor,
				     HINT_NONE, ctx->cmp_def) <= 0;
	if (between) {
		/*
		 * The current batch has already passed this key. Ask to
		 * restart it from the saved cursor so that this tuple is
		 * re-read. MVCC engines also abort the batch transaction
		 * via read tracking.
		 */
		ctx->restart_batch = true;
		a->any_between = true;
	}
	if (le) {
		/*
		 * The main loop has already passed this key: apply the
		 * statement to the building index now.
		 */
		int rc = ctx->vtab->replace_concurrent(ctx->index, old_tuple,
						       new_tuple);
		VERIFY(fiber()->csw == a->csw);
		if (rc != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
			a->done = true;
			return 0;
		}
	}
	index_build_abort_writers(new_tuple, in_txn(), ctx);
	VERIFY(fiber()->csw == a->csw);
	index_build_untrack(new_tuple, in_txn(), ctx);
	VERIFY(fiber()->csw == a->csw);
	return 0;
}

static int
index_build_on_prepare_stmt(struct trigger *base, void *event)
{
	assert(!index_build_txn_is_aborted());
	int csw = fiber()->csw;

	struct txn_stmt *stmt = (struct txn_stmt *)event;
	struct index_build_stmt_trigger *trigger =
		container_of(base, struct index_build_stmt_trigger, on_prepare);
	struct index_build_ctx *ctx = trigger->ctx;
	if (ctx->is_failed)
		goto out;

	trigger->has_run_on_prepare = true;

	struct index_build_on_prepare_arg arg = {
		.ctx = ctx, .csw = csw, .done = false, .any_between = false,
	};
	ctx->vtab->iter_stmt(stmt, index_build_on_prepare_f, &arg);
	assert(!index_build_txn_is_aborted() || arg.any_between);
out:
	VERIFY(fiber()->csw == csw);
	return 0;
}

static int
index_build_on_commit_stmt(struct trigger *base, void *event)
{
	(void)event;
	struct index_build_stmt_trigger *trigger =
		container_of(base, struct index_build_stmt_trigger, on_commit);
	index_build_stmt_trigger_delete(trigger);
	return 0;
}

/**
 * on_rollback trigger of a concurrent transaction's statement.
 *
 * If the statement never reached on_prepare, just drop its write point.
 * Otherwise undo the change applied to the building index at prepare
 * time (if any) and abort transactions that conflict with the restored
 * tuple.
 */
struct index_build_on_rollback_arg {
	struct index_build_ctx *ctx;
	int csw;
	bool has_run_on_prepare;
};

static int
index_build_on_rollback_f(struct tuple *old_tuple, struct tuple *new_tuple,
			   void *data)
{
	struct index_build_on_rollback_arg *a = data;
	if (a->ctx->is_failed)
		return 0;
	if (!a->has_run_on_prepare) {
		index_build_untrack(new_tuple, in_txn(), a->ctx);
		return 0;
	}
	if (old_tuple != NULL &&
	    tuple_validate(a->ctx->format, old_tuple) != 0) {
		a->ctx->is_failed = true;
		diag_move(diag_get(), &a->ctx->diag);
		return 0;
	}
	index_build_abort_writers(old_tuple, in_txn(), a->ctx);
	VERIFY(fiber()->csw == a->csw);
	int rc = a->ctx->vtab->rollback_concurrent(a->ctx->index, old_tuple,
						   new_tuple);
	VERIFY(fiber()->csw == a->csw);
	if (rc != 0) {
		a->ctx->is_failed = true;
		diag_move(diag_get(), &a->ctx->diag);
	}
	return 0;
}

static int
index_build_on_rollback_stmt(struct trigger *base, void *event)
{
	int csw = fiber()->csw;
	struct txn_stmt *stmt = (struct txn_stmt *)event;
	assert(stmt != NULL);

	struct index_build_stmt_trigger *trigger =
		container_of(base, struct index_build_stmt_trigger, on_rollback);
	index_build_stmt_trigger_delete(trigger);

	struct index_build_ctx *ctx = trigger->ctx;

	if (ctx->is_failed)
		goto out;

	struct index_build_on_rollback_arg arg = {
		.ctx = ctx, .csw = csw,
		.has_run_on_prepare = trigger->has_run_on_prepare,
	};
	ctx->vtab->iter_stmt(stmt, index_build_on_rollback_f, &arg);
out:
	VERIFY(fiber()->csw == csw);
	return 0;
}

/**
 * Encode and write the build cursor (the primary key of @a cursor tuple,
 * or nil for the start of the space) into _index_local as part of the
 * current batch transaction.
 */
static int
index_build_save_cursor(struct index_build_ctx *ctx, struct tuple *cursor)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	size_t sz = mp_sizeof_array(3);
	sz += mp_sizeof_uint(ctx->space_id);
	sz += mp_sizeof_uint(ctx->iid);
	uint32_t key_len = 0;
	const char *key = NULL;
	if (cursor != NULL) {
		key = tuple_extract_key(cursor, ctx->cmp_def, MULTIKEY_NONE,
					&key_len);
		if (key == NULL) {
			region_truncate(region, region_svp);
			return -1;
		}
		sz += key_len;
	} else {
		sz += mp_sizeof_nil();
	}

	char *buf = xregion_alloc(region, sz);
	char *p = mp_encode_array(buf, 3);
	p = mp_encode_uint(p, ctx->space_id);
	p = mp_encode_uint(p, ctx->iid);
	if (cursor != NULL) {
		memcpy(p, key, key_len);
		p += key_len;
	} else {
		p = mp_encode_nil(p);
	}

	int rc = box_replace(BOX_INDEX_LOCAL_ID, buf, p, NULL);
	region_truncate(region, region_svp);
	return rc;
}

/**
 * Batch transaction on_commit trigger: finalize the tuples inserted by
 * the main loop of this batch.
 */
static int
index_build_batch_on_commit(struct trigger *trigger, void *event)
{
	(void)event;
	struct index_build_batch_state *state = trigger->data;
	struct index_build_ctx *ctx = state->ctx;
	struct index_build_batch_tuple *entry;
	rlist_foreach_entry(entry, &state->inserted, in_inserted) {
		if (ctx->vtab->commit_main(ctx->index, entry->tuple) != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
			return 0;
		}
	}
	return 0;
}

/**
 * Batch transaction on_rollback trigger: undo the tuples inserted by the
 * main loop of this batch and drop the batch's write set entries.
 */
static int
index_build_batch_on_rollback(struct trigger *trigger, void *event)
{
	(void)event;
	int csw = fiber()->csw;
	struct index_build_batch_state *state = trigger->data;
	struct index_build_ctx *ctx = state->ctx;
	struct index_build_batch_tuple *entry;
	rlist_foreach_entry(entry, &state->inserted, in_inserted) {
		if (ctx->vtab->rollback_main(ctx->index, entry->tuple) != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
		}
	}
	rlist_foreach_entry(entry, &state->batch, in_ctx)
		index_build_untrack(entry->tuple, ctx->build_txn, ctx);
	VERIFY(fiber()->csw == csw);
	return 0;
}

/** Unref all tuples held in @a list. Entries live on the fiber region. */
static void
index_build_batch_list_unref(struct rlist *list)
{
	struct index_build_batch_tuple *entry, *tmp;
	rlist_foreach_entry_safe(entry, list, in_ctx, tmp)
		tuple_unref(entry->tuple);
}

/**
 * Run a single build batch: open a read-prepared transaction, read a
 * batch of tuples from the primary index starting from the saved cursor,
 * insert them into the building index and commit. On a conflict the
 * batch is retried; on a hard error the build is aborted.
 *
 * @retval 0 - the batch committed (or the end of the space was reached).
 * @retval 1 - the batch must be retried (a conflict restarted it).
 * @retval -1 - a hard error, the build is aborted.
 */
static int
index_build_run_batch(struct index_build_ctx *ctx, struct index *pk,
		      int yield_loops, bool *done)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	if (box_txn_begin() != 0) {
		region_truncate(region, region_svp);
		return -1;
	}
	if (box_txn_set_isolation(TXN_ISOLATION_READ_COMMITTED) != 0) {
		box_txn_rollback();
		region_truncate(region, region_svp);
		return -1;
	}
	struct txn *txn = in_txn();
	assert(txn != NULL);
	ctx->build_txn = txn;
	ctx->batch_cursor = NULL;
	ctx->restart_batch = false;

	struct index_build_batch_state *state =
		xregion_alloc_object(region, struct index_build_batch_state);
	state->ctx = ctx;
	rlist_create(&state->batch);
	rlist_create(&state->inserted);

	struct trigger on_commit;
	struct trigger on_rollback;
	trigger_create(&on_commit, index_build_batch_on_commit, state, NULL);
	trigger_create(&on_rollback, index_build_batch_on_rollback, state,
		       NULL);
	txn_on_commit(txn, &on_commit);
	txn_on_rollback(txn, &on_rollback);

	const char *key = NULL;
	uint32_t part_count = 0;
	if (ctx->cursor != NULL) {
		key = tuple_extract_key(ctx->cursor, ctx->cmp_def,
					MULTIKEY_NONE, NULL);
		if (key == NULL) {
			box_txn_rollback();
			ctx->build_txn = NULL;
			region_truncate(region, region_svp);
			return -1;
		}
		mp_decode_array(&key);
		part_count = ctx->cmp_def->part_count;
	}

	/*
	 * Continue strictly after the saved cursor: ITER_GE would re-read the
	 * cursor tuple, which is already in the building index.
	 */
	enum iterator_type it_type = ctx->cursor != NULL ? ITER_GT : ITER_GE;
	struct iterator *it = index_create_iterator(pk, it_type, key,
						     part_count);
	if (it == NULL) {
		box_txn_rollback();
		ctx->build_txn = NULL;
		region_truncate(region, region_svp);
		return -1;
	}
	struct tuple *tuple = NULL;
	struct tuple *last_tuple = NULL;
	struct index_build_batch_tuple *entry;
	int count = 0;
	int rc = 0;
	while (count < yield_loops &&
	       (rc = iterator_next(it, &tuple)) == 0 && tuple != NULL) {
		if (ctx->restart_batch)
			break;
		if (txn_check_can_continue(txn) != 0)
			break;
		if (tuple_validate(ctx->format,
					  tuple) != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
			break;
		}
		if (ctx->check_unique_constraint) {
			if (index_build_check_is_unique_secondary(
				    NULL, tuple, ctx->index, ctx) != 0) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
				break;
			}
			if (index_build_txn_is_aborted())
				break;
		}
		if (ctx->is_failed)
			break;
		/*
		 * Advance the current cursor and register the tuple in
		 * the write set so that concurrent transactions writing
		 * the same key are detected and aborted.
		 */
		ctx->batch_cursor = tuple;
		if (index_build_track(tuple, txn, ctx) != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
			break;
		}
		entry = xregion_alloc_object(region,
					     struct index_build_batch_tuple);
		entry->tuple = tuple;
		tuple_ref(entry->tuple);
		rlist_add_entry(&state->batch, entry, in_ctx);
		/*
		 * Insert the tuple into the building index immediately so
		 * that get_prepared() finds it when checking uniqueness for
		 * subsequent tuples in the same batch.
		 */
		if (ctx->vtab->insert_main(ctx->index, tuple) != 0) {
			if (!ctx->is_failed) {
				ctx->is_failed = true;
				diag_move(diag_get(), &ctx->diag);
			}
			break;
		}
		rlist_add_entry(&state->inserted, entry, in_inserted);
		last_tuple = tuple;
		count++;
	}
	iterator_delete(it);

	bool retry = false;
	if (rc != 0 && !ctx->is_failed && !ctx->restart_batch) {
		ctx->is_failed = true;
		diag_move(diag_get(), &ctx->diag);
	}

	if (ctx->is_failed) {
		box_txn_rollback();
		goto cleanup_fail;
	}

	if (count == 0 && !ctx->restart_batch) {
		/* End of the primary index reached. */
		*done = true;
		box_txn_rollback();
		ctx->build_txn = NULL;
		region_truncate(region, region_svp);
		return 0;
	}

	if (ctx->restart_batch) {
		box_txn_rollback();
		retry = true;
		goto cleanup_retry;
	}

	/*
	 * Persist the cursor progress in _index_local so that the batch
	 * transaction lands in the WAL and the limbo: if a prepared tuple
	 * read by this batch is rolled back, the batch is rolled back too
	 * and the cursor reverts to the previously saved position.
	 */
	if (index_build_save_cursor(ctx, last_tuple) != 0) {
		ctx->is_failed = true;
		diag_move(diag_get(), &ctx->diag);
		box_txn_rollback();
		goto cleanup_fail;
	}
	/*
	 * Abort concurrent transactions conflicting with the batch tuples
	 * and drop the batch's own write set entries: the tuples are now
	 * in the index, the unique check will catch future conflicts.
	 */
	rlist_foreach_entry(entry, &state->batch, in_ctx) {
		index_build_abort_writers(entry->tuple, txn, ctx);
		index_build_untrack(entry->tuple, txn, ctx);
	}

	if (box_txn_commit() != 0) {
		if (ctx->is_failed)
			rc = -1;
		else
			retry = true;
		index_build_batch_list_unref(&state->batch);
		ctx->build_txn = NULL;
		region_truncate(region, region_svp);
		return retry ? 1 : -1;
	}

	/* Success: advance the saved cursor. */
	if (ctx->cursor != NULL)
		tuple_unref(ctx->cursor);
	ctx->cursor = last_tuple;
	tuple_ref(ctx->cursor);

	index_build_batch_list_unref(&state->batch);
	ctx->build_txn = NULL;
	region_truncate(region, region_svp);
	return 0;

cleanup_fail:
	index_build_batch_list_unref(&state->batch);
	ctx->build_txn = NULL;
	region_truncate(region, region_svp);
	return -1;
cleanup_retry:
	index_build_batch_list_unref(&state->batch);
	ctx->build_txn = NULL;
	region_truncate(region, region_svp);
	return 1;
}

int
index_build(struct space *src_space, struct index *new_index,
	    struct tuple_format *new_format,
	    bool check_unique_constraint,
	    const struct index_build_vtab *vtab,
	    bool can_yield, int yield_loops, bool need_wal_sync)
{
	/*
	 * The yielding (concurrent) path opens its own batch transactions, so
	 * it must run with no transaction attached to the fiber. The
	 * non-yielding recovery path instead runs inside the recovery
	 * transaction.
	 */
	if (can_yield)
		assert(in_txn() == NULL);

	struct index *pk = index_find(src_space, 0);
	if (pk == NULL)
		return -1;

	struct errinj *inj = errinj(ERRINJ_BUILD_INDEX, ERRINJ_INT);
	if (inj != NULL && inj->iparam == (int)new_index->def->iid) {
		diag_set(ClientError, ER_INJECTION, "build index");
		return -1;
	}

	inj = errinj(ERRINJ_BUILD_INDEX_DISABLE_YIELD, ERRINJ_BOOL);
	if (inj != NULL && inj->bparam == true)
		can_yield = false;

	/*
	 * The yielding path writes the build cursor to _index_local via the
	 * journal, so it needs a working journal (with a sync method). During
	 * bootstrap and recovery the journal is not fully initialized yet:
	 * fall back to the simple non-yielding build there.
	 */
	if (can_yield &&
	    (current_journal == NULL || current_journal->sync == NULL))
		can_yield = false;

	if (new_index->def->iid == 0)
		check_unique_constraint = false;

	struct index_build_ctx ctx;
	ctx.index = new_index;
	ctx.format = new_format;
	ctx.check_unique_constraint = check_unique_constraint;
	ctx.vtab = vtab;
	ctx.cmp_def = pk->def->key_def;
	ctx.cursor = NULL;
	ctx.batch_cursor = NULL;
	ctx.build_txn = NULL;
	ctx.restart_batch = false;
	ctx.is_failed = false;
	ctx.space_id = src_space->def->id;
	ctx.iid = new_index->def->iid;

	if (can_yield) {
		index_build_write_set_new(&ctx.write_set);
		rlist_create(&ctx.stmt_triggers);
		diag_create(&ctx.diag);

		trigger_create(&ctx.on_replace, index_build_on_replace, &ctx,
			       NULL);
		trigger_add(&src_space->on_replace, &ctx.on_replace);

		if (need_wal_sync && journal_is_initialized(current_journal)) {
			if (journal_sync(NULL) != 0)
				goto cleanup;
			if (txn_limbo_flush(&txn_limbo) != 0)
				goto cleanup;
		}
	}

	int rc = 0;
	if (can_yield) {
		bool done = false;
		while (!done && !ctx.is_failed) {
			int batch_rc = index_build_run_batch(&ctx, pk,
							     yield_loops,
							     &done);
			if (batch_rc < 0) {
				rc = -1;
				break;
			}
			/* batch_rc == 1: retry the same batch. */
			ERROR_INJECT_YIELD(ERRINJ_BUILD_INDEX_DELAY);
			if (fiber_is_cancelled()) {
				diag_set(FiberIsCancelled);
				rc = -1;
				break;
			}
		}
		if (rc != 0) {
			if (ctx.is_failed)
				diag_move(&ctx.diag, diag_get());
			else if (diag_is_empty(diag_get()))
				diag_set(ClientError, ER_PROC_LUA,
					 "index build failed");
		}
	} else {
		/*
		 * Non-yielding path: build the index in one run, with no
		 * concurrent transaction handling. Used during recovery and
		 * for unordered primary keys.
		 */
		struct iterator *it = index_create_iterator(pk, ITER_ALL, NULL,
							     0);
		if (it == NULL)
			goto cleanup;
		struct tuple *tuple;
		while ((rc = iterator_next(it, &tuple)) == 0 &&
		       tuple != NULL) {
			if (tuple_validate(new_format, tuple) != 0) {
				rc = -1;
				break;
			}
			if (vtab->insert_main(new_index, tuple) != 0) {
				rc = -1;
				break;
			}
			ERROR_INJECT_DOUBLE(ERRINJ_BUILD_INDEX_TIMEOUT,
					    inj->dparam > 0,
					    thread_sleep(inj->dparam));
		}
		iterator_delete(it);
	}

cleanup:
	if (can_yield) {
		index_build_write_set_free(&ctx);

		struct index_build_stmt_trigger *trg, *tmp;
		rlist_foreach_entry_safe(trg, &ctx.stmt_triggers, in_ctx, tmp) {
			if (!trg->has_run_on_prepare)
				trigger_clear(&trg->on_prepare);
			trigger_clear(&trg->on_commit);
			trigger_clear(&trg->on_rollback);
		}
		diag_destroy(&ctx.diag);
		trigger_clear(&ctx.on_replace);
		if (ctx.cursor != NULL)
			tuple_unref(ctx.cursor);
	}
	return rc;
}
