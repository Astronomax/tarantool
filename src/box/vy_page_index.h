#ifndef INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H
#define INCLUDES_TARANTOOL_BOX_VY_PAGE_INDEX_H

#include "vy_stmt.h"
#include "iterator_type.h"
#include "small/rlist.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum { VY_PAGE_INDEX_VERSION = 3 };

struct vy_run;
struct xlog;
struct xlog_cursor;

/** Address of one independently checksummed xlog transaction. */
struct vy_index_ref {
	uint64_t offset;
	uint32_t size;
	uint32_t unpacked_size;
};

struct vy_index_node;

/** Global TX cache for either internal nodes or leaves. */
struct vy_index_cache {
	struct rlist lru;
	struct rlist *buckets;
	size_t bucket_count;
	size_t mem_used;
	size_t mem_quota;
	int64_t hit, miss, evict;
};

struct vy_page_index_env {
	struct vy_index_cache inner, leaf;
	uint32_t resident_depth, disk_depth, fanout, leaf_size;
	int64_t resident_bytes, read_bytes, read_ops, write_bytes;
};

struct vy_page_index {
	int fd;
	struct vy_index_ref root;
	struct vy_index_node *resident;
	uint32_t key_part_count;
	struct rlist cached;
	uint32_t height;
	uint64_t root_stride;
	uint32_t leaf_size;
	uint32_t fanout;
	/** Sample i is the minimum key of data page i * sample_stride. */
	uint32_t sample_stride;
	uint32_t sample_count;
	const char **samples;
};

/**
 * State transferred from TX to a reader on the first cache miss.
 * A search by key is complete when page_no and info are known. A search
 * by page number uses the same descent with key = vy_entry_none().
 */
struct vy_index_lookup {
	struct vy_run *run;
	struct key_def *cmp_def;
	struct vy_entry key;
	enum iterator_type type;
	uint32_t page_no;
	bool equal;
	uint8_t inner_reads, leaf_reads;
	struct vy_index_ref ref;
	uint32_t level;
	uint32_t first;
	uint64_t stride;
	/** Nodes private to this request, offered to the TX cache on return. */
	struct rlist fetched;
};

struct vy_page_info;
void
vy_page_index_env_create(struct vy_page_index_env *env);
void
vy_page_index_env_destroy(struct vy_page_index_env *env);

/** Returns true if info (or EOF) was resolved from resident/cached nodes. */
bool
vy_page_index_lookup(struct vy_index_lookup *lookup, struct vy_run *run,
		     struct key_def *cmp_def, struct vy_entry key,
		     enum iterator_type type, uint32_t page_no,
		     struct vy_page_info *info);

/** Complete a miss on the reader. Only immutable run state is shared. */
int
vy_page_index_read(struct vy_index_lookup *lookup, struct vy_page_info *info);
void
vy_page_index_lookup_done(struct vy_index_lookup *lookup);

/** Synchronous lookup for a compaction stream, with its own leaf buffer. */
int
vy_page_index_stream_lookup(struct vy_run *run, struct vy_index_node **leaf,
			    struct key_def *cmp_def, struct vy_entry key,
			    enum iterator_type type, uint32_t *page_no,
			    struct vy_page_info *info);
void
vy_page_index_stream_close(struct vy_index_node *leaf);

/** Lower bound in the resident sample, in units of sample_stride pages. */
uint32_t
vy_page_index_sample_bound(struct vy_run *run, struct key_def *cmp_def,
			   struct vy_entry key);

/** Borrow the nearest sampled key at or before a page. Does not yield. */
const char *
vy_page_index_sample_key(struct vy_run *run, uint32_t page_no);

struct vy_page_index_writer;
struct vy_page_index_writer *
vy_page_index_writer_new(struct vy_run *run, struct xlog *log);
void
vy_page_index_writer_delete(struct vy_page_index_writer *w);

/** Transfer ownership of the page's min key on success. */
int
vy_page_index_writer_append(struct vy_page_index_writer *w, char *key,
			    const struct vy_page_info *page);
int
vy_page_index_writer_finish(struct vy_page_index_writer *w,
			    struct vy_index_ref *root);
int
vy_page_index_write_manifest(struct vy_page_index_writer *w, struct vy_run *run,
			     struct vy_index_ref root, off_t run_info_offset);

struct vy_index_trailer {
	off_t start, run_info, manifest, end;
};

/** Returns 1 for format 3, 0 for a legacy index, or -1 on error. */
int
vy_page_index_read_trailer(struct vy_run *run, struct xlog_cursor *cursor,
			   struct vy_index_trailer *trailer);
int
vy_page_index_open(struct vy_run *run, struct xlog_cursor *cursor,
		   struct key_def *cmp_def, const struct vy_index_trailer *trailer,
		   bool verify);

void
vy_page_index_destroy(struct vy_run *run);

#if defined(__cplusplus)
} /* extern "C" */
#endif
#endif
