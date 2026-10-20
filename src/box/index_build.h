#ifndef TARANTOOL_BOX_INDEX_BUILD_H_INCLUDED
#define TARANTOOL_BOX_INDEX_BUILD_H_INCLUDED

#include <stdbool.h>

#include "space.h"
#include "tuple_format.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * Engine-specific operations used by the generic index build procedure.
 * The building index is only ever mutated through these methods, and only
 * at the moment a transaction touching it becomes prepared (see the RFC).
 */
struct index_build_vtab {
	/**
	 * Insert a tuple read by the main build loop into the building
	 * index. Called at the build batch prepare time.
	 */
	int (*insert_main)(struct index *index, struct tuple *tuple);
	/**
	 * Commit a tuple inserted by insert_main once the build batch
	 * transaction is committed. No-op for engines that need no
	 * deferred commit (e.g. memtx).
	 */
	int (*commit_main)(struct index *index, struct tuple *tuple);
	/**
	 * Roll back a tuple inserted by insert_main when the build batch
	 * transaction is rolled back after it has been prepared.
	 */
	int (*rollback_main)(struct index *index, struct tuple *tuple);
	/**
	 * Look up a key in the building index, returning a tuple that has
	 * already been prepared. Used for the unique constraint check.
	 */
	struct tuple *(*get_prepared)(struct index *index, const char *key,
				      uint32_t part_count);
	/**
	 * Apply a concurrent statement (old_tuple -> new_tuple) to the
	 * building index. Called at the concurrent transaction prepare
	 * time, for tuples the main loop has already passed.
	 */
	int (*replace_concurrent)(struct index *index, struct tuple *old_tuple,
				  struct tuple *new_tuple);
	/**
	 * Roll back a statement applied by replace_concurrent when the
	 * concurrent transaction is rolled back after it has been
	 * prepared.
	 */
	int (*rollback_concurrent)(struct index *index, struct tuple *old_tuple,
				   struct tuple *new_tuple);
};

/**
 * Default no-op implementations of the vtab methods, used by engines that
 * don't need a particular operation.
 */
int
generic_index_build_insert_main(struct index *index, struct tuple *tuple);

int
generic_index_build_commit_main(struct index *index, struct tuple *tuple);

int
generic_index_build_rollback_main(struct index *index, struct tuple *tuple);

int
generic_index_build_replace_concurrent(struct index *index,
				       struct tuple *old_tuple,
				       struct tuple *new_tuple);

int
generic_index_build_rollback_concurrent(struct index *index,
					struct tuple *old_tuple,
					struct tuple *new_tuple);

/**
 * Build @a new_index on top of @a src_space using the generic, engine-agnostic
 * procedure described in the RFC "Index Build 4.x".
 *
 * The procedure must be called with no transaction attached to the fiber
 * (fiber()->txn == NULL): the caller detaches the DDL transaction for the
 * duration of the build.
 *
 * @a check_unique_constraint enables the secondary key unique check (always
 * enabled for secondary indexes).
 *
 * @a can_yield allows the procedure to yield between batches and to handle
 * concurrent transactions; when false, the index is built in one run with no
 * concurrency handling.
 *
 * @a need_wal_sync, when true, flushes the journal and the limbo before the
 * build starts, so that only transactions started after the build begins can
 * affect it.
 */
int
index_build(struct space *src_space, struct index *new_index,
	    struct tuple_format *new_format,
	    bool check_unique_constraint,
	    const struct index_build_vtab *vtab,
	    bool can_yield, int yield_loops, bool need_wal_sync);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_INDEX_BUILD_H_INCLUDED */
