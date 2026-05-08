#ifndef INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H
#define INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H

#include "key_def.h"
#include "iterator_type.h"
#include "vy_stmt.h"
#include "small/rlist.h"
#include "small/matras.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_page_info;

struct vy_page_index_entry {
	/** Index in sorted array. */
	int32_t idx;
	/** Minimal key stored in the page. */
	char *min_key;
	/** Comparison hint of the min key. */
	hint_t min_key_hint;
};

/* {{{ Page Index Cache */

struct vy_page_index_cache;

struct vy_page_index_cache_node {
	struct vy_page_index_cache *cache;
	struct vy_page_index_entry entry;
	struct rlist in_lru;
};

enum {
	/* Max number of deletes that are made by cleanup action per one
	 * cache operation */
	VY_PAGE_INDEX_CACHE_CLEANUP_MAX_STEPS = 10,
};

static inline int
vy_page_index_entry_compare(const struct vy_page_index_entry *a,
			    const struct vy_page_index_entry *b,
			    struct key_def *cmp_def)
{
	(void)cmp_def;
	return (int)a->idx - (int)b->idx;
}

static inline int
vy_page_index_entry_compare_with_key(const struct vy_page_index_entry *entry,
			     struct vy_entry key, struct key_def *cmp_def)
{
	return vy_entry_compare_with_raw_key(key, entry->min_key,
					     entry->min_key_hint, cmp_def);
}

static inline int
vy_page_index_cache_tree_cmp(struct vy_page_index_cache_node *a,
			     struct vy_page_index_cache_node *b,
			     struct key_def *cmp_def)
{
	(void)cmp_def;
	return vy_page_index_entry_compare(&a->entry, &b->entry, cmp_def);
}

static inline int
vy_page_index_cache_tree_key_cmp(struct vy_page_index_cache_node *a,
				 struct vy_entry b,
				 struct key_def *cmp_def)
{
	return -vy_page_index_entry_compare_with_key(&a->entry, b, cmp_def);
}

#define VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE 512//(16 * 1024)

#define BPS_TREE_NAME vy_page_index_cache_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_COMPARE(a, b, cmp_def) \
	vy_page_index_cache_tree_cmp(a, b, cmp_def)
#define BPS_TREE_COMPARE_KEY(a, b, cmp_def) \
	vy_page_index_cache_tree_key_cmp(a, b, cmp_def)
#define bps_tree_elem_t struct vy_page_index_cache_node *
#define bps_tree_key_t struct vy_entry
#define bps_tree_arg_t struct key_def *
#define BPS_TREE_IS_IDENTICAL(a, b) ((a)->entry.idx == (b)->entry.idx)

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t
#undef BPS_TREE_IS_IDENTICAL

struct vy_page_index_cache_env {
	struct rlist cache_lru;
	struct mempool cache_node_mempool;
	struct matras_allocator allocator;
	/**
	 * Memory occupied by matras extents backing the cache tree.
	 * This memory is allocated/freed together with the allocator,
	 * not with individual cache nodes.
	 */
	size_t tree_mem_used;
	/** In-memory .btree levels (vy_page_index_btree), all runs on this env. */
	size_t btree_mem_used;
	size_t mem_used;
	size_t mem_quota;
	struct {
		int64_t hit;
		int64_t miss;
		int64_t evict;
	} stat;
	struct {
		int64_t read_bytes;
		int64_t read_ops;
		int64_t write_bytes;
	} io;
};

struct vy_page_index_cache {
	/** Number of pages in the run. */
	uint32_t page_count;
	struct key_def *cmp_def;
	struct vy_page_index_cache_tree cache_tree;
	struct vy_page_index_cache_env *env;
	/** Matras stats for cache_tree. */
	struct matras_stats matras_stats;
};

void
vy_page_index_cache_env_create(struct vy_page_index_cache_env *env,
			       struct slab_cache *slabc);

void
vy_page_index_cache_env_destroy(struct vy_page_index_cache_env *env);

/* }}} Page Index Cache */

/* {{{ B-tree */

struct vy_page_index_btree_node;

/** B-tree structure. */
struct vy_page_index_btree {
	/** Number of pages in the run. */
	uint32_t page_count;
	/** Key definition for comparison. */
	struct key_def *cmp_def;
	/** For I/O accounting. */
	struct vy_page_index_cache_env *env;
	/** Root node offset in file. */
	uint64_t root_offset;
	/** Offset of btree binary payload in file. */
	uint64_t data_offset;
	/** Filepath of the .btree file. */
	char *filepath;
	/** File descriptor for reading, lifetime is bound to run object. */
	int fd;
	/** Depth of the in memory part of the tree. */
	uint32_t in_memory_depth;
	/** In memory root node. */
	struct vy_page_index_btree_node *root;
};

/* }}} B-tree */

/* {{{ Page Info Cache */

struct vy_page_info_cache;

#define VY_PAGE_INFO_BLOCK 64

struct vy_page_info_block {
	uint32_t l;
	uint32_t r;
	struct vy_page_info *data[VY_PAGE_INFO_BLOCK];
};

struct vy_page_info_cache_node {
	struct vy_page_info_cache *cache;
	struct vy_page_info_block block;
	/** Number of iterators currently pinned to this node. */
	uint32_t pin_count;
	struct rlist in_lru;
};

enum {
	/* Max number of deletes that are made by cleanup action per one
	 * cache operation */
	VY_PAGE_INFO_CACHE_CLEANUP_MAX_STEPS = 10,
};

static inline int
vy_page_info_cache_tree_cmp(struct vy_page_info_cache_node *a,
			    struct vy_page_info_cache_node *b,
			    void *unused)
{
	(void)unused;
	return (int)a->block.l - (int)b->block.l;
}

static inline int
vy_page_info_cache_tree_key_cmp(struct vy_page_info_cache_node *a,
				uint32_t b, void *unused)
{
	(void)unused;
	return (int)a->block.r - (int)b;
}

#define VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE 512//(16 * 1024)

#define BPS_TREE_NAME vy_page_info_cache_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_COMPARE(a, b, unused) \
	vy_page_info_cache_tree_cmp(a, b, unused)
#define BPS_TREE_COMPARE_KEY(a, b, unused) \
	vy_page_info_cache_tree_key_cmp(a, b, unused)
#define bps_tree_elem_t struct vy_page_info_cache_node *
#define bps_tree_key_t uint32_t
#define bps_tree_arg_t void *
#define BPS_TREE_IS_IDENTICAL(a, b) ((a)->block.l == (b)->block.l)

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t
#undef BPS_TREE_IS_IDENTICAL

struct vy_page_info_cache_env {
	struct rlist cache_lru;
	struct mempool cache_node_mempool;
	struct matras_allocator allocator;
	/**
	 * Memory occupied by matras extents backing the cache tree.
	 * This memory is allocated/freed together with the allocator,
	 * not with individual cache nodes.
	 */
	size_t tree_mem_used;
	size_t mem_used;
	size_t mem_quota;
	struct {
		int64_t hit;
		int64_t miss;
		int64_t evict;
		int64_t pinned;
	} stat;
	struct {
		int64_t read_bytes;
		int64_t read_ops;
		int64_t write_bytes;
	} io;
};

struct vy_page_info_cache {
	/** Number of pages in the run. */
	uint32_t page_count;
	struct vy_page_info_cache_tree cache_tree;
	struct vy_page_info_cache_env *env;
	/** Matras stats for cache_tree. */
	struct matras_stats matras_stats;
};

void
vy_page_info_cache_env_create(struct vy_page_info_cache_env *env,
			      struct slab_cache *slabc);

void
vy_page_info_cache_env_destroy(struct vy_page_info_cache_env *env);

/* }}} Page Info Cache */

/* {{{ Page Info Array */

struct vy_page_index_array {
	struct key_def *cmp_def;
	uint32_t page_count;
	/** Cache. */
	struct vy_page_info_cache cache;
	/** Source of truth. */
	int index_fd;
	const char *index_filepath;
	int index_offsets_fd;
	const char *index_offsets_filepath;
	/**
	 * Transitional in-memory source for blocks that are not loaded from disk
	 * yet. If set, entries can point directly to this array.
	 */
	struct vy_page_info *page_info;
};

struct vy_page_index {
	/** Number of pages in the run. */
	uint32_t page_count;
	/** Cache. */
	struct vy_page_index_cache cache;
	/** Source of truth. */
	struct vy_page_index_btree btree;
	/** Page info array. */
	struct vy_page_index_array page_info;
};

struct vy_page_index_array_iterator {
	struct vy_page_info_cache_tree_iterator cache_it;
	struct vy_page_info_cache_node *node;
	uint32_t page_no;
};

struct vy_page_index_array_iterator
vy_page_index_array_invalid_iterator(void);

struct vy_page_info *
vy_page_index_array_iterator_get(struct vy_page_index_array_iterator *it);

int
vy_page_index_array_iterator_next(struct vy_page_index_array *array,
				 struct vy_page_index_array_iterator *it);

int
vy_page_index_array_iterator_prev(struct vy_page_index_array *array,
				 struct vy_page_index_array_iterator *it);

struct vy_page_info *
vy_page_index_array_iterator_get(struct vy_page_index_array_iterator *it);

void
vy_page_index_array_iterator_close(struct vy_page_index_array_iterator *it);

int
vy_page_index_recover(struct vy_page_index *index,
		      const char *index_path,
		      const char *index_btree_path,
		      const char *index_offsets_path,
		      struct vy_page_index_cache_env *page_index_cache_env,
		      struct vy_page_info_cache_env *page_info_cache_env,
		      struct key_def *cmp_def,
		      struct vy_page_info *page_info_array, uint32_t page_count);

int
vy_page_index_write(struct vy_page_index *index,
		    struct vy_page_info *page_info,
		    struct vy_page_index_entry *entries,
		    uint32_t page_count,
		    const char *index_path,
		    const char *index_btree_path,
		    const char *index_offsets_path,
		    struct vy_page_index_cache_env *page_index_cache_env,
		    struct vy_page_info_cache_env *page_info_cache_env,
		    struct key_def *cmp_def);

/* }}} Page Info Array */

/* {{{ Page Index */

void
vy_page_index_destroy(struct vy_page_index *index);
int
vy_page_index_find_page(struct vy_page_index *index, struct vy_entry key,
			enum iterator_type itype,
			uint32_t *result, bool *equal_key);

/**
 * Get iterator positioned at page_no.
 * @retval 0 Success, iterator initialized.
 * @retval 1 Requested page doesn't exist.
 * @retval -1 Read/IO error.
 */
int
vy_page_index_get_page(struct vy_page_index *index, uint32_t page_no,
		       struct vy_page_index_array_iterator *it);

/* }}} Page Index */

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H */
