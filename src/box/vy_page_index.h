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

#define VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE (16 * 1024)

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
	size_t mem_used;
	size_t mem_quota;
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

/** B-tree structure. */
struct vy_page_index_btree {
	/** Number of pages in the run. */
	uint32_t page_count;
	/** Key definition for comparison. */
	struct key_def *cmp_def;
	/** Root node offset in file. */
	uint64_t root_offset;
	/** Offset of btree binary payload in file. */
	uint64_t data_offset;

	char *filepath;
	/** File descriptor for reading, lifetime is bound to run object. */
	int fd;
};

struct vy_page_index {
	/** Number of pages in the run. */
	uint32_t page_count;
	/** Cache. */
	struct vy_page_index_cache cache;
	/** Source of truth. */
	struct vy_page_index_btree btree;
};

/* }}} B-tree */

int
vy_page_index_btree_read_meta(const char *filepath, uint64_t *root_offset,
			      uint64_t *data_offset);

void
vy_page_index_create(struct vy_page_index *index,
		     struct vy_page_index_cache_env *env,
		     struct key_def *cmp_def, uint64_t root_offset,
		     const char *filepath, uint32_t page_count);

void
vy_page_index_destroy(struct vy_page_index *index);

int
vy_page_index_btree_build(struct vy_page_index_btree *btree,
			  struct vy_page_index_entry *pages, uint32_t page_count,
			  struct key_def *cmp_def, const char *filepath);

int
vy_page_index_btree_open(struct vy_page_index_btree *btree);

int
vy_page_index_find_page(struct vy_page_index *index, struct vy_entry key,
			enum iterator_type itype,
			uint32_t *result, bool *equal_key);

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H */
