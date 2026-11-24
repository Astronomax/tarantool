#include "vy_page_index.h"

#include <pmatomic.h>

#include "fio.h"
#include "mp_util.h"
#include "vy_run.h"
#include "xrow.h"

/*
 * Format 3 .index stores child nodes before parents, followed by the run
 * info, manifest and EOF.
 * Each node is a separate xlog transaction. Child links contain both
 * compressed and unpacked lengths, just like data pages in the .run.
 * The uncompressed manifest ends with a magic and two 64-bit offsets
 * pointing to the run info (including its bloom filter) and the manifest.
 * The manifest array contains fanout, leaf capacity, row count, unpacked
 * and compressed data sizes, root reference, sample stride, keys and trailer.
 *
 * A node body is {VY_BTREE_DATA: bin}. Inside the bin, fixed width slots
 * are followed by concatenated MessagePack keys. All integers in the bin
 * are big endian. A leaf slot contains a key offset and the five fields of
 * vy_page_info. A compact leaf uses a 64-bit base offset, 32-bit data
 * offsets relative to that base, and 16-bit lengths, counts and key offsets.
 * Wide leaves retain full-width fields. Each internal slot links to a child
 * and contains the minimum key of that child's first data page.
 */
enum {
	VY_INDEX_REF_SIZE = 16,
	VY_INDEX_COMPACT_SLOT = 14,
	VY_INDEX_WIDE_SLOT = 28,
	VY_INDEX_INNER_SLOT = 4 + VY_INDEX_REF_SIZE,
	VY_INDEX_MAX_SAMPLES = 128,
};

struct vy_index_node {
	struct rlist in_lru, in_bucket, in_run;
	struct vy_run *run;
	struct vy_index_ref ref;
	uint32_t first, count, level, slot_size;
	uint64_t base_offset;
	const char *slots;
	/** Populated only for permanently resident descendants. */
	struct vy_index_node **children;
	/** The decoded xrow, including the slot and key arrays. */
	char data[];
};

static inline int
vy_index_invalid(const char *file, const char *reason)
{
	diag_set(ClientError, ER_INVALID_INDEX_FILE, file, reason);
	return -1;
}

static inline int
vy_index_oom(size_t size)
{
	diag_set(OutOfMemory, size, "malloc", "Vinyl page index");
	return -1;
}

static uint32_t
vy_index_div_up(uint32_t n, uint64_t d)
{
	return 1 + (n - 1) / d;
}

static char *
vy_index_encode_ref(char *p, struct vy_index_ref ref)
{
	p = mp_store_u64(p, ref.offset);
	p = mp_store_u32(p, ref.size);
	return mp_store_u32(p, ref.unpacked_size);
}

static struct vy_index_ref
vy_index_decode_ref(const char *p)
{
	struct vy_index_ref ref;
	ref.offset = mp_load_u64(&p);
	ref.size = mp_load_u32(&p);
	ref.unpacked_size = mp_load_u32(&p);
	return ref;
}

static bool
vy_index_ref_valid(struct vy_index_ref ref, uint64_t limit)
{
	return ref.size > XLOG_FIXHEADER_SIZE && ref.unpacked_size > 0 &&
	       ref.offset <= limit && ref.size <= limit - ref.offset;
}

static char *
vy_index_encode_page(char *p, const struct vy_page_info *info,
		     bool compact, uint64_t base)
{
	if (compact) {
		p = mp_store_u32(p, info->offset - base);
		p = mp_store_u16(p, info->size);
		p = mp_store_u16(p, info->unpacked_size);
		p = mp_store_u16(p, info->row_count);
		return mp_store_u16(p, info->row_index_offset);
	}
	p = mp_store_u64(p, info->offset);
	p = mp_store_u32(p, info->size);
	p = mp_store_u32(p, info->unpacked_size);
	p = mp_store_u32(p, info->row_count);
	return mp_store_u32(p, info->row_index_offset);
}

static const char *
vy_index_slot(const struct vy_index_node *node, uint32_t i)
{
	return node->slots + i * node->slot_size;
}

static uint32_t
vy_index_key_offset(const struct vy_index_node *node, const char **slot)
{
	return node->slot_size == VY_INDEX_COMPACT_SLOT ?
	       mp_load_u16(slot) : mp_load_u32(slot);
}

static const char *
vy_index_key(const struct vy_index_node *node, uint32_t i)
{
	const char *slot = vy_index_slot(node, i);
	return node->slots + vy_index_key_offset(node, &slot);
}

static void
vy_index_decode_page(const struct vy_index_node *node, uint32_t i,
		     struct vy_page_info *info)
{
	const char *p = vy_index_slot(node, i);
	if (node->slot_size == VY_INDEX_COMPACT_SLOT) {
		p += 2;
		info->offset = node->base_offset + mp_load_u32(&p);
		info->size = mp_load_u16(&p);
		info->unpacked_size = mp_load_u16(&p);
		info->row_count = mp_load_u16(&p);
		info->row_index_offset = mp_load_u16(&p);
		return;
	}
	p += 4;
	info->offset = mp_load_u64(&p);
	info->size = mp_load_u32(&p);
	info->unpacked_size = mp_load_u32(&p);
	info->row_count = mp_load_u32(&p);
	info->row_index_offset = mp_load_u32(&p);
}

/** Write one independently addressable xrow and record its actual extent. */
static int
vy_index_write_row(struct xlog *log, char *body, char *end,
		   struct vy_index_ref *ref)
{
	struct xrow_header row;
	memset(&row, 0, sizeof(row));
	row.type = VY_INDEX_BTREE;
	row.bodycnt = 1;
	row.body[0].iov_base = body;
	row.body[0].iov_len = end - body;
	ref->offset = log->offset;
	xlog_tx_begin(log);
	ssize_t size = xlog_write_row(log, &row);
	if (size < 0) {
		xlog_tx_rollback(log);
		return -1;
	}
	if (xlog_tx_commit(log) < 0 || xlog_flush(log) < 0)
		return -1;
	if (size > UINT32_MAX || log->offset - ref->offset > UINT32_MAX)
		return vy_index_invalid(log->filename, "Index node too large");
	ref->size = log->offset - ref->offset;
	ref->unpacked_size = size;
	return 0;
}

struct vy_index_item {
	char *key;
	union {
		struct vy_page_info page;
		struct vy_index_ref child;
	};
};

struct vy_index_build_node {
	struct vy_index_item *items;
	uint32_t count;
};

/* At fanout 2, 33 levels suffice for UINT32_MAX pages. */
struct vy_page_index_writer {
	struct xlog *log;
	uint32_t fanout, leaf_size, height;
	struct vy_index_build_node levels[33];
	uint32_t pages, sample_stride, sample_count;
	char *samples[VY_INDEX_MAX_SAMPLES];
};

static const char vy_index_magic[] = "VYIDX003";
enum { VY_INDEX_TRAILER_SIZE = 24 };

static int
vy_index_write_node(struct vy_page_index_writer *w, uint32_t level,
		    struct vy_index_ref *result)
{
	struct xlog *log = w->log;
	struct vy_index_build_node *node = &w->levels[level];
	struct vy_index_item *items = node->items;
	uint32_t n = node->count;
	assert(n > 0);
	struct region *region = &fiber()->gc;
	size_t svp = region_used(region);
	uint64_t base = level == 0 ? items[0].page.offset : 0;
	bool compact = level == 0;
	size_t keys_size = 0;
	for (uint32_t i = 0; i < n; i++) {
		keys_size += mp_len(items[i].key);
		const struct vy_page_info *p = &items[i].page;
		compact = compact && p->offset - base <= UINT32_MAX &&
			  p->size <= UINT16_MAX && p->unpacked_size <= UINT16_MAX &&
			  p->row_count <= UINT16_MAX;
	}
	compact = compact && n * VY_INDEX_COMPACT_SLOT + keys_size <= UINT16_MAX;
	size_t slot_size = level > 0 ? VY_INDEX_INNER_SLOT :
			   compact ? VY_INDEX_COMPACT_SLOT : VY_INDEX_WIDE_SLOT;
	size_t header = level > 0 ? 0 : compact ? 9 : 1;
	size_t blob_size = header + n * slot_size + keys_size;
	if (blob_size > UINT32_MAX - 64) {
		vy_index_invalid(log->filename, "Index node too large");
		goto fail;
	}
	size_t size = mp_sizeof_map(1) + mp_sizeof_uint(VY_BTREE_DATA) +
		      mp_sizeof_bin(blob_size);
	char *body = region_alloc(region, size);
	if (body == NULL) {
		vy_index_oom(size);
		goto fail;
	}
	char *p = mp_encode_map(body, 1);
	p = mp_encode_uint(p, VY_BTREE_DATA);
	p = mp_encode_binl(p, blob_size);
	if (level == 0) {
		*p++ = slot_size;
		if (compact)
			p = mp_store_u64(p, base);
	}
	char *slots = p;
	char *keys = slots + n * slot_size;
	p = slots;
	for (uint32_t i = 0; i < n; i++) {
		p = compact ? mp_store_u16(p, keys - slots) :
			      mp_store_u32(p, keys - slots);
		if (level == 0)
			p = vy_index_encode_page(p, &items[i].page, compact, base);
		else
			p = vy_index_encode_ref(p, items[i].child);
		const char *key = items[i].key;
		size_t len = mp_len(key);
		memcpy(keys, key, len);
		keys += len;
	}
	assert(keys == body + size);
	if (vy_index_write_row(log, body, keys, result) != 0)
		goto fail;
	region_truncate(region, svp);
	return 0;
fail:
	region_truncate(region, svp);
	return -1;
}

/** Number of data pages covered by one slot of the root. */
static uint64_t
vy_index_geometry(struct vy_page_index *pi, uint32_t page_count)
{
	uint64_t capacity = pi->leaf_size;
	uint64_t stride = 1;
	pi->height = 1;
	while (capacity < page_count) {
		stride = capacity;
		capacity *= pi->fanout;
		pi->height++;
	}
	return stride;
}

struct vy_page_index_writer *
vy_page_index_writer_new(struct vy_run *run, struct xlog *log)
{
	struct vy_page_index_writer *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		vy_index_oom(sizeof(*w));
		return NULL;
	}
	w->log = log;
	w->fanout = run->env->page_index_env.fanout;
	w->leaf_size = run->env->page_index_env.leaf_size;
	w->sample_stride = 1;
	return w;
}

void
vy_page_index_writer_delete(struct vy_page_index_writer *w)
{
	if (w == NULL)
		return;
	for (uint32_t i = 0; i < w->height; i++) {
		struct vy_index_build_node *node = &w->levels[i];
		for (uint32_t j = 0; j < node->count; j++)
			free(node->items[j].key);
		free(node->items);
	}
	for (uint32_t i = 0; i < w->sample_count; i++)
		free(w->samples[i]);
	free(w);
}

/** Flush a full or final node, transferring its fence key to the parent. */
static int
vy_index_writer_flush(struct vy_page_index_writer *w, uint32_t level,
		      struct vy_index_item *parent)
{
	struct vy_index_build_node *node = &w->levels[level];
	if (vy_index_write_node(w, level, &parent->child) != 0)
		return -1;
	parent->key = node->items[0].key;
	for (uint32_t i = 1; i < node->count; i++)
		free(node->items[i].key);
	node->count = 0;
	return 0;
}

/* On success the node owns item->key. On failure the caller still owns it. */
static int
vy_index_writer_push(struct vy_page_index_writer *w, uint32_t level,
		     struct vy_index_item *item)
{
	assert(level < lengthof(w->levels));
	struct vy_index_build_node *node = &w->levels[level];
	uint32_t capacity = level == 0 ? w->leaf_size : w->fanout;
	if (node->items == NULL) {
		node->items = malloc(capacity * sizeof(*node->items));
		if (node->items == NULL)
			return vy_index_oom(capacity * sizeof(*node->items));
		w->height = level + 1;
	}
	if (node->count == capacity) {
		struct vy_index_item parent;
		if (vy_index_writer_flush(w, level, &parent) != 0)
			return -1;
		if (vy_index_writer_push(w, level + 1, &parent) != 0) {
			free(parent.key);
			return -1;
		}
	}
	node->items[node->count++] = *item;
	return 0;
}

int
vy_page_index_writer_append(struct vy_page_index_writer *w, char *key,
			    const struct vy_page_info *page)
{
	if (w->pages == UINT32_MAX)
		return vy_index_invalid(w->log->filename, "Too many pages");
	struct vy_index_item item = {.key = key, .page = *page};
	if (vy_index_writer_push(w, 0, &item) != 0)
		return -1;
	/* Double the sample spacing in place as the run grows. */
	if (w->sample_count == VY_INDEX_MAX_SAMPLES &&
	    w->pages % w->sample_stride == 0) {
		for (uint32_t i = 0; i < w->sample_count; i += 2) {
			free(w->samples[i + 1]);
			w->samples[i / 2] = w->samples[i];
		}
		w->sample_count /= 2;
		w->sample_stride *= 2;
	}
	if (w->pages % w->sample_stride == 0)
		w->samples[w->sample_count++] = mp_dup(key);
	w->pages++;
	return 0;
}

int
vy_page_index_writer_finish(struct vy_page_index_writer *w,
			    struct vy_index_ref *root)
{
	for (uint32_t i = 0; i < w->height; i++) {
		struct vy_index_item parent;
		if (vy_index_writer_flush(w, i, &parent) != 0)
			return -1;
		if (i + 1 == w->height) {
			*root = parent.child;
			free(parent.key);
			return 0;
		}
		if (vy_index_writer_push(w, i + 1, &parent) != 0) {
			free(parent.key);
			return -1;
		}
	}
	unreachable();
}

int
vy_page_index_write_manifest(struct vy_page_index_writer *w, struct vy_run *run,
			     struct vy_index_ref root, off_t run_info_offset)
{
	struct xlog *log = w->log;
	uint32_t ns = w->sample_count;
	uint32_t step = w->sample_stride;
	assert(w->pages == run->info.page_count);
	assert(ns == vy_index_div_up(w->pages, step));
	size_t size = mp_sizeof_map(1) + mp_sizeof_uint(VY_BTREE_DATA) +
		      mp_sizeof_array(9) + mp_sizeof_uint(step) +
		      mp_sizeof_uint(w->fanout) +
		      mp_sizeof_uint(w->leaf_size) + mp_sizeof_uint(run->count.rows) +
		      mp_sizeof_uint(run->count.bytes) +
		      mp_sizeof_uint(run->count.bytes_compressed) +
		      mp_sizeof_bin(VY_INDEX_REF_SIZE) + mp_sizeof_array(ns) +
		      mp_sizeof_bin(VY_INDEX_TRAILER_SIZE);
	for (uint32_t i = 0; i < ns; i++)
		size += mp_len(w->samples[i]);
	char *body = region_alloc(&fiber()->gc, size);
	if (body == NULL)
		return vy_index_oom(size);
	char *p = mp_encode_map(body, 1);
	p = mp_encode_uint(p, VY_BTREE_DATA);
	p = mp_encode_array(p, 9);
	p = mp_encode_uint(p, w->fanout);
	p = mp_encode_uint(p, w->leaf_size);
	p = mp_encode_uint(p, run->count.rows);
	p = mp_encode_uint(p, run->count.bytes);
	p = mp_encode_uint(p, run->count.bytes_compressed);
	p = vy_index_encode_ref(mp_encode_binl(p, VY_INDEX_REF_SIZE), root);
	p = mp_encode_uint(p, step);
	p = mp_encode_array(p, ns);
	for (uint32_t i = 0; i < ns; i++) {
		const char *key = w->samples[i];
		size_t len = mp_len(key);
		memcpy(p, key, len);
		p += len;
	}
	p = mp_encode_binl(p, VY_INDEX_TRAILER_SIZE);
	memcpy(p, vy_index_magic, 8);
	p = mp_store_u64(p + 8, run_info_offset);
	p = mp_store_u64(p, log->offset);
	assert(p == body + size);
	log->opts.no_compression = true;
	struct vy_index_ref unused;
	return vy_index_write_row(log, body, p, &unused);
}

static const char *
vy_index_row_data(struct xrow_header *row, enum mp_type type)
{
	if (row->type != VY_INDEX_BTREE || row->bodycnt != 1)
		return NULL;
	const char *p = row->body[0].iov_base;
	if (mp_typeof(*p) != MP_MAP || mp_decode_map(&p) != 1 ||
	    mp_typeof(*p) != MP_UINT || mp_decode_uint(&p) != VY_BTREE_DATA ||
	    mp_typeof(*p) != type)
		return NULL;
	return p;
}

static int
vy_index_pread(struct vy_run *run, int fd, void *buf, size_t size, uint64_t off)
{
	ssize_t n = fio_pread(fd, buf, size, off);
	if (n < 0) {
		diag_set(SystemError, "reading Vinyl page index");
		return -1;
	}
	pm_atomic_fetch_add(&run->env->page_index_env.read_bytes, n);
	pm_atomic_fetch_add(&run->env->page_index_env.read_ops, 1);
	if ((size_t)n != size)
		return vy_index_invalid(fio_filename(fd), "Short index read");
	return 0;
}

static struct vy_index_node *
vy_index_read_node(struct vy_run *run, struct vy_index_ref ref,
		   uint32_t first, uint32_t level, uint64_t stride)
{
	struct vy_page_index *pi = &run->page_index;
	uint32_t n = vy_index_div_up(run->info.page_count - first, stride);
	n = MIN(n, level == 0 ? pi->leaf_size : pi->fanout);
	if (n * (level == 0 ? VY_INDEX_COMPACT_SLOT : VY_INDEX_INNER_SLOT) >=
	    ref.unpacked_size)
		goto invalid;
	struct vy_index_node *node = malloc(sizeof(*node) + ref.unpacked_size);
	if (node == NULL) {
		vy_index_oom(sizeof(*node) + ref.unpacked_size);
		return NULL;
	}
	struct region *region = &fiber()->gc;
	size_t svp = region_used(region);
	char *buf = region_alloc(region, ref.size);
	if (buf == NULL) {
		vy_index_oom(ref.size);
		goto fail;
	}
	if (vy_index_pread(run, pi->fd, buf, ref.size, ref.offset) != 0)
		goto fail;
	ZSTD_DStream *z = vy_run_env_get_zdctx(run->env);
	if (z == NULL || xlog_tx_decode(buf, buf + ref.size, node->data,
					node->data + ref.unpacked_size, z) != 0)
		goto fail;
	region_truncate(region, svp);
	const char *p = node->data;
	const char *end = p + ref.unpacked_size;
	struct xrow_header row;
	if (xrow_decode(&row, &p, end, true) != 0)
		goto fail;
	p = vy_index_row_data(&row, MP_BIN);
	if (p == NULL)
		goto bad_node;
	uint32_t size;
	const char *slots = mp_decode_bin(&p, &size);
	if (p != end || size == 0)
		goto bad_node;
	node->slot_size = VY_INDEX_INNER_SLOT;
	node->base_offset = 0;
	if (level == 0) {
		node->slot_size = (uint8_t)*slots++;
		if (node->slot_size == VY_INDEX_COMPACT_SLOT) {
			if (end - slots < 8)
				goto bad_node;
			node->base_offset = mp_load_u64(&slots);
		} else if (node->slot_size != VY_INDEX_WIDE_SLOT) {
			goto bad_node;
		}
	}
	size_t table = n * node->slot_size;
	if ((size_t)(end - slots) <= table)
		goto bad_node;
	node->slots = slots;
	node->run = run;
	node->ref = ref;
	node->first = first;
	node->level = level;
	node->count = n;
	node->children = NULL;
	/* Validate offsets before any search can use this immutable block. */
	const char *key = slots + table;
	for (uint32_t i = 0; i < n; i++) {
		p = vy_index_slot(node, i);
		if (vy_index_key_offset(node, &p) != (size_t)(key - slots) ||
		    key == end || mp_typeof(*key) != MP_ARRAY)
			goto bad_node;
		const char *parts = key;
		if (mp_check(&key, end) != 0 ||
		    mp_decode_array(&parts) != pi->key_part_count)
			goto bad_node;
		if (level > 0) {
			if (!vy_index_ref_valid(vy_index_decode_ref(p), ref.offset))
				goto bad_node;
		} else {
			struct vy_page_info info;
			vy_index_decode_page(node, i, &info);
			if (info.offset < node->base_offset || info.row_count == 0 ||
			    info.size <= XLOG_FIXHEADER_SIZE ||
			    info.row_index_offset >= info.unpacked_size ||
			    info.row_count > info.unpacked_size / sizeof(uint32_t))
				goto bad_node;
		}
	}
	if (key != end)
		goto bad_node;
	return node;
bad_node:
	vy_index_invalid(fio_filename(pi->fd), "Malformed B+ tree node");
fail:
	region_truncate(region, svp);
	free(node);
	return NULL;
invalid:
	vy_index_invalid(fio_filename(pi->fd), "Invalid B+ tree node extent");
	return NULL;
}

static size_t
vy_index_node_size(const struct vy_index_node *node)
{
	return sizeof(*node) + node->ref.unpacked_size;
}

static void
vy_index_free_resident(struct vy_index_node *node)
{
	if (node == NULL)
		return;
	if (node->children != NULL) {
		for (uint32_t i = 0; i < node->count; i++)
			vy_index_free_resident(node->children[i]);
		free(node->children);
	}
	free(node);
}

/**
 * Load the configured resident levels. During forced recovery also verify
 * the remaining subtrees, releasing each node as soon as it is checked.
 */
static int
vy_index_preload(struct vy_run *run, struct vy_index_ref ref, uint32_t first,
		 uint32_t level, uint64_t stride, uint32_t depth, bool verify,
		 struct vy_index_node **out)
{
	struct vy_index_node *node = vy_index_read_node(run, ref, first,
							level, stride);
	if (node == NULL)
		return -1;
	if (depth > 1) {
		node->children = calloc(node->count, sizeof(*node->children));
		if (node->children == NULL) {
			vy_index_oom(node->count * sizeof(*node->children));
			goto fail;
		}
		run->page_index_size += node->count * sizeof(*node->children);
	}
	if (level > 0 && (depth > 1 || verify)) {
		uint64_t step = level == 1 ? 1 : stride / run->page_index.fanout;
		for (uint32_t i = 0; i < node->count; i++) {
			struct vy_index_ref child =
				vy_index_decode_ref(vy_index_slot(node, i) + 4);
			struct vy_index_node **dst = depth > 1 ?
						     &node->children[i] : NULL;
			if (vy_index_preload(run, child, first + i * stride,
					     level - 1, step, depth > 1 ? depth - 1 : 0,
					     verify, dst) != 0)
				goto fail;
		}
	}
	if (depth > 0) {
		run->page_index_size += vy_index_node_size(node);
		*out = node;
	} else {
		free(node);
	}
	return 0;
fail:
	vy_index_free_resident(node);
	return -1;
}

int
vy_page_index_read_trailer(struct vy_run *run, struct xlog_cursor *cursor,
			   struct vy_index_trailer *trailer)
{
	struct stat st;
	if (fstat(cursor->fd, &st) != 0) {
		diag_set(SystemError, "stat %s", cursor->name);
		return -1;
	}
	trailer->start = xlog_cursor_pos(cursor);
	trailer->end = st.st_size - (off_t)sizeof(uint32_t);
	char tail[VY_INDEX_TRAILER_SIZE];
	if (trailer->end < (off_t)sizeof(tail))
		return 0;
	if (vy_index_pread(run, cursor->fd, tail, sizeof(tail),
			   trailer->end - sizeof(tail)) != 0)
		return -1;
	if (memcmp(tail, vy_index_magic, 8) != 0)
		return 0;
	const char *p = tail + 8;
	uint64_t info = mp_load_u64(&p);
	uint64_t manifest = mp_load_u64(&p);
	if (info <= (uint64_t)trailer->start || info >= manifest ||
	    manifest >= (uint64_t)trailer->end - sizeof(tail))
		return vy_index_invalid(cursor->name, "Invalid page index trailer");
	trailer->run_info = info;
	trailer->manifest = manifest;
	return 1;
}

int
vy_page_index_open(struct vy_run *run, struct xlog_cursor *cursor,
		   struct key_def *cmp_def, const struct vy_index_trailer *trailer,
		   bool verify)
{
	struct vy_page_index *pi = &run->page_index;
	uint32_t n = run->info.page_count;
	if (n == 0)
		goto invalid;
	xlog_cursor_seek_tx(cursor, trailer->manifest,
			    trailer->end - trailer->manifest);
	struct xrow_header row;
	int rc = xlog_cursor_next_tx(cursor);
	if (rc == 0)
		rc = xlog_cursor_next_row(cursor, &row);
	if (rc < 0)
		return -1;
	if (rc != 0 || ibuf_used(&cursor->tx_cursor) != 0)
		goto invalid;
	const char *p = vy_index_row_data(&row, MP_ARRAY);
	if (p == NULL || mp_decode_array(&p) != 9)
		goto invalid;
	uint64_t fields[5];
	for (size_t i = 0; i < lengthof(fields); i++) {
		if (mp_typeof(*p) != MP_UINT)
			goto invalid;
		fields[i] = mp_decode_uint(&p);
	}
	if (fields[0] < 2 || fields[0] > 128 ||
	    fields[1] == 0 || fields[1] > 1024 ||
	    fields[2] < n || fields[2] > INT64_MAX ||
	    fields[3] > INT64_MAX || fields[4] > INT64_MAX)
		goto invalid;
	pi->fanout = fields[0];
	pi->leaf_size = fields[1];
	run->count.pages = n;
	run->count.rows = fields[2];
	run->count.bytes = fields[3];
	run->count.bytes_compressed = fields[4];
	uint32_t len;
	if (mp_typeof(*p) != MP_BIN)
		goto invalid;
	const char *link = mp_decode_bin(&p, &len);
	if (len != VY_INDEX_REF_SIZE)
		goto invalid;
	pi->root = vy_index_decode_ref(link);
	if (!vy_index_ref_valid(pi->root, trailer->run_info) ||
	    pi->root.offset < (uint64_t)trailer->start)
		goto invalid;
	if (mp_typeof(*p) != MP_UINT)
		goto invalid;
	uint64_t step = mp_decode_uint(&p);
	if (step == 0 || step > n ||
	    vy_index_div_up(n, step) > VY_INDEX_MAX_SAMPLES)
		goto invalid;
	pi->sample_stride = step;
	pi->sample_count = vy_index_div_up(n, pi->sample_stride);
	if (mp_typeof(*p) != MP_ARRAY || mp_decode_array(&p) != pi->sample_count)
		goto invalid;
	const char *keys = p;
	for (uint32_t i = 0; i < pi->sample_count; i++) {
		if (mp_typeof(*p) != MP_ARRAY)
			goto invalid;
		const char *parts = p;
		if (mp_decode_array(&parts) != cmp_def->part_count)
			goto invalid;
		mp_next(&p);
	}
	size_t keys_size = p - keys;
	if (mp_typeof(*p) != MP_BIN ||
	    mp_decode_binl(&p) != VY_INDEX_TRAILER_SIZE ||
	    memcmp(p, vy_index_magic, 8) != 0)
		goto invalid;
	p += 8;
	if (mp_load_u64(&p) != (uint64_t)trailer->run_info ||
	    mp_load_u64(&p) != (uint64_t)trailer->manifest ||
	    xlog_cursor_pos(cursor) != trailer->end)
		goto invalid;
	size_t size = pi->sample_count * sizeof(*pi->samples) + keys_size;
	pi->samples = malloc(size);
	if (pi->samples == NULL)
		return vy_index_oom(size);
	char *storage = (char *)(pi->samples + pi->sample_count);
	memcpy(storage, keys, keys_size);
	p = storage;
	for (uint32_t i = 0; i < pi->sample_count; i++) {
		pi->samples[i] = p;
		mp_next(&p);
	}
	if (xlog_cursor_next_tx(cursor) != 1 || !xlog_cursor_is_eof(cursor))
		goto invalid;
	run->page_index_size = sizeof(*pi) + size;
	run->page_index_disk_size = trailer->end + sizeof(uint32_t);
	pi->key_part_count = cmp_def->part_count;
	pi->fd = cursor->fd;
	cursor->fd = -1;
	pi->root_stride = vy_index_geometry(pi, n);
	struct vy_page_index_env *env = &run->env->page_index_env;
	uint32_t depth = MIN(env->resident_depth,
			     pi->height - MIN(env->disk_depth, pi->height));
	if ((depth > 0 || verify) &&
	    vy_index_preload(run, pi->root, 0, pi->height - 1, pi->root_stride,
			     depth, verify, &pi->resident) != 0) {
		run->page_index_size = 0;
		return -1;
	}
	pm_atomic_fetch_add(&env->resident_bytes, run->page_index_size);
	return 0;
invalid:
	return vy_index_invalid(cursor->name, "Invalid page index manifest");
}

void
vy_page_index_env_create(struct vy_page_index_env *env)
{
	memset(env, 0, sizeof(*env));
	rlist_create(&env->inner.lru);
	rlist_create(&env->leaf.lru);
}

void
vy_page_index_env_destroy(struct vy_page_index_env *env)
{
	assert(rlist_empty(&env->inner.lru) && rlist_empty(&env->leaf.lru));
	free(env->inner.buckets);
	free(env->leaf.buckets);
}

static struct vy_index_cache *
vy_index_cache(struct vy_run *run, uint32_t level)
{
	struct vy_page_index_env *env = &run->env->page_index_env;
	return level == 0 ? &env->leaf : &env->inner;
}

/* A leaf's first page number is known even if its parent was evicted. */
static size_t
vy_index_bucket(struct vy_index_cache *cache, struct vy_run *run,
		uint32_t first, uint32_t level)
{
	uint32_t h = first * 2654435761U + (uint32_t)run->id * 31 + level;
	return (h ^ (h >> 16)) % cache->bucket_count;
}

static struct vy_index_node *
vy_index_cache_find(struct vy_index_cache *cache, struct vy_run *run,
		    uint32_t first, uint32_t level)
{
	if (cache->buckets == NULL)
		return NULL;
	size_t h = vy_index_bucket(cache, run, first, level);
	struct vy_index_node *node;
	rlist_foreach_entry(node, &cache->buckets[h], in_bucket) {
		if (node->run == run && node->first == first &&
		    node->level == level) {
			rlist_move(&cache->lru, &node->in_lru);
			return node;
		}
	}
	return NULL;
}

static void
vy_index_cache_remove(struct vy_index_cache *cache, struct vy_index_node *node)
{
	cache->mem_used -= vy_index_node_size(node);
	rlist_del(&node->in_lru);
	rlist_del(&node->in_run);
	rlist_del(&node->in_bucket);
	free(node);
}

static void
vy_index_cache_put(struct vy_index_node *node)
{
	struct vy_run *run = node->run;
	struct vy_index_cache *cache = vy_index_cache(run, node->level);
	size_t size = vy_index_node_size(node);
	size_t count = MAX((size_t)1, MIN((size_t)65536, cache->mem_quota / 2048));
	size_t overhead = count * sizeof(*cache->buckets);
	if (size + overhead > cache->mem_quota ||
	    vy_index_cache_find(cache, run, node->first, node->level) != NULL) {
		free(node);
		return;
	}
	if (cache->buckets == NULL) {
		cache->buckets = malloc(overhead);
		if (cache->buckets == NULL) {
			free(node);
			return;
		}
		for (size_t i = 0; i < count; i++)
			rlist_create(&cache->buckets[i]);
		cache->bucket_count = count;
		cache->mem_used = overhead;
	}
	while (cache->mem_used > cache->mem_quota - size) {
		struct vy_index_node *old = rlist_last_entry(&cache->lru,
						struct vy_index_node, in_lru);
		vy_index_cache_remove(cache, old);
		cache->evict++;
	}
	size_t h = vy_index_bucket(cache, run, node->first, node->level);
	rlist_add(&cache->buckets[h], &node->in_bucket);
	rlist_add(&run->page_index.cached, &node->in_run);
	rlist_add(&cache->lru, &node->in_lru);
	cache->mem_used += size;
}

void
vy_page_index_destroy(struct vy_run *run)
{
	struct vy_page_index *pi = &run->page_index;
	while (!rlist_empty(&pi->cached)) {
		struct vy_index_node *node = rlist_first_entry(&pi->cached,
						struct vy_index_node, in_run);
		struct vy_index_cache *cache = vy_index_cache(run, node->level);
		vy_index_cache_remove(cache, node);
		if (rlist_empty(&cache->lru)) {
			free(cache->buckets);
			cache->buckets = NULL;
			cache->mem_used = 0;
		}
	}
	vy_index_free_resident(pi->resident);
	free(pi->samples);
	if (pi->fd >= 0) {
		pm_atomic_fetch_sub(&run->env->page_index_env.resident_bytes,
				    run->page_index_size);
		close(pi->fd);
	}
	memset(pi, 0, sizeof(*pi));
	pi->fd = -1;
	rlist_create(&pi->cached);
}

static void
vy_index_lookup_start(struct vy_index_lookup *it, struct vy_run *run,
		      struct key_def *cmp_def, struct vy_entry key,
		      enum iterator_type type, uint32_t page)
{
	struct vy_page_index *pi = &run->page_index;
	it->run = run;
	it->cmp_def = cmp_def;
	it->key = key;
	it->type = type;
	it->page_no = page;
	it->equal = false;
	it->inner_reads = it->leaf_reads = 0;
	it->first = 0;
	it->level = pi->height - 1;
	it->stride = pi->root_stride;
	it->ref = pi->root;
	rlist_create(&it->fetched);
}

static uint32_t
vy_index_choose_slot(struct vy_index_lookup *it, struct vy_index_node *node)
{
	if (it->key.stmt == NULL)
		return (it->page_no - it->first) / it->stride;
	bool upper = it->type == ITER_GT || it->type == ITER_LE;
	uint32_t lo = 0, hi = node->count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const char *fence = vy_index_key(node, mid);
		int cmp = vy_entry_compare_with_raw_key(it->key, fence,
							HINT_NONE, it->cmp_def);
		it->equal |= cmp == 0;
		if (cmp > 0 || (upper && cmp == 0))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0 && node->level == 0 && it->first == 0 &&
	    iterator_direction(it->type) < 0)
		return UINT32_MAX;
	return lo == 0 ? 0 : lo - 1;
}

static uint32_t
vy_index_step(struct vy_index_lookup *it, struct vy_index_node *node,
	      struct vy_page_info *info)
{
	uint32_t slot = vy_index_choose_slot(it, node);
	if (slot == UINT32_MAX) {
		it->page_no = it->run->info.page_count;
		return slot;
	}
	assert(slot < node->count);
	if (it->level == 0) {
		it->page_no = it->first + slot;
		vy_index_decode_page(node, slot, info);
	} else {
		it->ref = vy_index_decode_ref(vy_index_slot(node, slot) + 4);
		it->first += slot * it->stride;
		it->level--;
		it->stride = it->level == 0 ? 1 :
			     it->stride / it->run->page_index.fanout;
	}
	return slot;
}

static uint32_t
vy_index_sample_bound(struct vy_page_index *pi, struct key_def *cmp_def,
		      struct vy_entry key, bool upper, bool *equal)
{
	uint32_t lo = 0, hi = pi->sample_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		int cmp = vy_entry_compare_with_raw_key(key, pi->samples[mid],
							HINT_NONE, cmp_def);
		*equal |= cmp == 0;
		if (cmp > 0 || (upper && cmp == 0))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/** Try the leaf cache directly when the missing ancestor covers many pages. */
static bool
vy_index_lookup_cached_leaf(struct vy_index_lookup *it, struct vy_page_info *info)
{
	struct vy_run *run = it->run;
	struct vy_page_index *pi = &run->page_index;
	struct vy_index_cache *cache = &run->env->page_index_env.leaf;
	if (cache->buckets == NULL)
		return false;
	uint32_t first;
	if (it->key.stmt == NULL) {
		first = it->page_no - it->page_no % pi->leaf_size;
	} else {
		bool upper = it->type == ITER_GT || it->type == ITER_LE;
		uint32_t n = vy_index_sample_bound(pi, it->cmp_def, it->key,
						  upper, &it->equal);
		uint32_t lo = n == 0 ? 0 : (n - 1) * pi->sample_stride;
		uint32_t end = n == pi->sample_count ? run->info.page_count :
			       n * pi->sample_stride;
		first = lo - lo % pi->leaf_size;
		if (end - first > pi->leaf_size)
			return false;
	}
	struct vy_index_node *node = vy_index_cache_find(cache, run, first, 0);
	if (node == NULL)
		return false;
	cache->hit++;
	it->first = first;
	it->stride = 1;
	it->level = 0;
	vy_index_step(it, node, info);
	return true;
}

bool
vy_page_index_lookup(struct vy_index_lookup *it, struct vy_run *run,
		     struct key_def *cmp_def, struct vy_entry key,
		     enum iterator_type type, uint32_t page_no,
		     struct vy_page_info *info)
{
	vy_index_lookup_start(it, run, cmp_def, key, type, page_no);
	struct vy_index_node *node = run->page_index.resident;
	for (;;) {
		if (node == NULL) {
			struct vy_index_cache *cache = vy_index_cache(run, it->level);
			node = vy_index_cache_find(cache, run, it->first, it->level);
			if (node == NULL) {
				return it->level > 0 &&
				       vy_index_lookup_cached_leaf(it, info);
			}
			cache->hit++;
		}
		uint32_t slot = vy_index_step(it, node, info);
		if (node->level == 0)
			return true;
		node = node->children == NULL ? NULL : node->children[slot];
	}
}

int
vy_page_index_read(struct vy_index_lookup *it, struct vy_page_info *info)
{
	for (;;) {
		struct vy_index_node *node = vy_index_read_node(it->run, it->ref,
					it->first, it->level, it->stride);
		if (node == NULL)
			return -1;
		bool leaf = node->level == 0;
		vy_index_step(it, node, info);
		if (leaf)
			it->leaf_reads++;
		else
			it->inner_reads++;
		/* Quotas are immutable for the lifetime of the environment. */
		size_t quota = vy_index_cache(it->run, node->level)->mem_quota;
		if (vy_index_node_size(node) <= quota)
			rlist_add(&it->fetched, &node->in_run);
		else
			free(node);
		if (leaf)
			return 0;
	}
}

void
vy_page_index_lookup_done(struct vy_index_lookup *it)
{
	struct vy_page_index_env *env = &it->run->env->page_index_env;
	env->inner.miss += it->inner_reads;
	env->leaf.miss += it->leaf_reads;
	while (!rlist_empty(&it->fetched)) {
		struct vy_index_node *node = rlist_shift_entry(&it->fetched,
						struct vy_index_node, in_run);
		vy_index_cache_put(node);
	}
}

int
vy_page_index_stream_lookup(struct vy_run *run, struct vy_index_node **leaf,
			    struct key_def *cmp_def, struct vy_entry key,
			    enum iterator_type type, uint32_t *page_no,
			    struct vy_page_info *info)
{
	struct vy_index_node *node = *leaf;
	if (key.stmt == NULL && node != NULL && *page_no >= node->first &&
	    *page_no - node->first < node->count) {
		vy_index_decode_page(node, *page_no - node->first, info);
		return 0;
	}
	struct vy_index_lookup it;
	vy_index_lookup_start(&it, run, cmp_def, key, type, *page_no);
	node = run->page_index.resident;
	for (;;) {
		bool owned = node == NULL;
		if (owned) {
			node = vy_index_read_node(run, it.ref, it.first,
						  it.level, it.stride);
			if (node == NULL)
				return -1;
		}
		uint32_t slot = vy_index_step(&it, node, info);
		if (node->level == 0) {
			if (owned) {
				free(*leaf);
				*leaf = node;
			}
			*page_no = it.page_no;
			return 0;
		}
		struct vy_index_node *child = node->children == NULL ? NULL :
					      node->children[slot];
		if (owned)
			free(node);
		node = child;
	}
}

void
vy_page_index_stream_close(struct vy_index_node *leaf)
{
	free(leaf);
}

uint32_t
vy_page_index_sample_bound(struct vy_run *run, struct key_def *cmp_def,
			   struct vy_entry key)
{
	bool equal = false;
	return vy_index_sample_bound(&run->page_index, cmp_def, key, false, &equal);
}

const char *
vy_page_index_sample_key(struct vy_run *run, uint32_t page_no)
{
	struct vy_page_index *pi = &run->page_index;
	return pi->samples[page_no / pi->sample_stride];
}
