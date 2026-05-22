#include "vy_page_index.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <pmatomic.h>

#include "cbus.h"

#include "diag.h"
#include "errcode.h"
#include "fiber.h"
#include "fio.h"
#include "mp_util.h"
#include "replication.h"
#include "trivia/util.h"
#include "xlog.h"
#include "xrow.h"

#include "small/ibuf.h"
#include "small/region.h"
#include "vy_run.h"
#include "vy_run_proto.h"

#define XLOG_META_TYPE_INDEX "INDEX"

static int
vy_page_index_btree_find_chain(struct vy_page_index_btree *btree,
			       struct vy_entry key, bool lower_bound,
			       struct vy_page_index_entry *next,
			       struct vy_page_index_entry *prev,
			       bool *equal_key);

static int
vy_page_index_array_read_block(struct vy_page_index_array *array,
			       uint32_t block_idx,
			       struct vy_page_info_block *result);

/** Cbus task for reading btree chain on cache miss. */
struct vy_page_index_btree_find_task {
	struct cbus_call_msg base;
	struct vy_page_index_btree *btree;
	struct vy_entry key;
	bool lower_bound;
	bool equal_key;
	struct vy_page_index_entry next;
	struct vy_page_index_entry prev;
};

static int
vy_page_index_btree_find_cb(struct cbus_call_msg *base)
{
	struct vy_page_index_btree_find_task *task =
		(struct vy_page_index_btree_find_task *)base;
	task->equal_key = false;
	memset(&task->next, 0, sizeof(task->next));
	memset(&task->prev, 0, sizeof(task->prev));
	/*
	 * The caller must open and preload the shared B-tree before COIO.
	 * The callback may only read from disk and use immutable B-tree state.
	 */
	return vy_page_index_btree_find_chain(task->btree, task->key,
					      task->lower_bound,
					      &task->next, &task->prev,
					      &task->equal_key);
}

/** Cbus task for reading a page_info block on cache miss. */
struct vy_page_info_block_read_task {
	struct cbus_call_msg base;
	struct vy_page_index_array *array;
	uint32_t block_idx;
	struct vy_page_info_block block;
};

static int
vy_page_info_block_read_cb(struct cbus_call_msg *base)
{
	struct vy_page_info_block_read_task *task =
		(struct vy_page_info_block_read_task *)base;
	return vy_page_index_array_read_block(task->array, task->block_idx,
					      &task->block);
}

/* {{{ B Tree */

/** B-tree order - maximum number of keys in a node. */
#define VY_PAGE_INDEX_BTREE_ORDER 64
#define XLOG_META_TYPE_BTREE "BTREE"

/** B-tree node types. */
enum vy_page_index_btree_node_type {
	VY_PAGE_INDEX_BTREE_NODE_LEAF = 0,
	VY_PAGE_INDEX_BTREE_NODE_INTERNAL = 1,
};

#define VY_PAGE_INDEX_BTREE_MAX_DEPTH 32

/**
 * B-tree node structure.
 * Internal nodes also contain data (vy_page_index_entry).
 */
struct vy_page_index_btree_node {
	/** Node type: leaf or internal. */
	enum vy_page_index_btree_node_type type;
	/** Number of keys (vy_page_index_entry) in this node. */
	uint32_t key_count;
	/** Separators. */
	struct vy_page_index_entry *keys;
	/**
	 * Array of child node offsets (only for internal nodes).
	 * Size is key_count + 1.
	 */
	union {
		/* For on disk part of the tree. */
		uint64_t *offsets;
		/* For in memory part of the tree. */
		struct vy_page_index_btree_node **nodes;
	} children;
	bool children_in_memory;
	/**
	 * If true, this node is a shallow copy of an in-memory node.
	 * Internal allocations (keys, children) are not owned and
	 * must not be freed — destroy is a no-op.
	 */
	bool borrowed;
	/** Offset of this node in the file. */
	uint64_t offset;
};

/** Ancestor on the iterator path (ownership transferred via move). */
struct vy_page_index_btree_path_entry {
	struct vy_page_index_btree_node node;
	/** Index of the child we descended into in @a node. */
	uint32_t child;
};

struct vy_page_index_btree_iterator {
	/** File offset of the node that contains the element. */
	uint64_t node_offset;
	/** Index inside node->keys[]. */
	uint32_t pos;
	/**
	 * Owned leaf node after lower/upper bound or last(). Reused by next/prev
	 * while the iterator stays inside the same leaf.
	 */
	bool has_current_node;
	struct vy_page_index_btree_node current_node;
	/**
	 * Path from root to the current node (excluding the current node).
	 * Each entry is the parent node and the child index we descended into.
	 * Nodes are moved into the path on descent, never copied.
	 */
	uint32_t depth;
	struct vy_page_index_btree_path_entry path[VY_PAGE_INDEX_BTREE_MAX_DEPTH];
};

/** Memory owned by @a page (struct + min_key). */
static inline size_t
vy_page_info_memory(const struct vy_page_info *page)
{
	size_t size = sizeof(*page);
	if (page->min_key != NULL)
		size += mp_len(page->min_key);
	return size;
}

/**
 * Memory of a single node body (struct + keys), without child links
 * or descendant subtrees. Used to size one on-disk node when measuring
 * the full tree for btree_memory_factor.
 */
static size_t
vy_page_index_btree_node_footprint(const struct vy_page_index_btree_node *node)
{
	size_t s = sizeof(struct vy_page_index_btree_node);
	if (node->keys != NULL) {
		s += (size_t)node->key_count * sizeof(*node->keys);
		for (uint32_t i = 0; i < node->key_count; i++) {
			if (node->keys[i].min_key != NULL)
				s += mp_len(node->keys[i].min_key);
		}
	}
	return s;
}

/** Memory owned by the in-memory .btree subtree rooted at @a node. */
static size_t
vy_page_index_btree_subtree_memory(const struct vy_page_index_btree_node *node)
{
	size_t s = vy_page_index_btree_node_footprint(node);
	if (node->type == VY_PAGE_INDEX_BTREE_NODE_LEAF)
		return s;
	if (node->children_in_memory) {
		uint32_t n = node->key_count + 1;
		s += (size_t)n * sizeof(*node->children.nodes);
		for (uint32_t i = 0; i < n; i++) {
			if (node->children.nodes[i] != NULL)
				s += vy_page_index_btree_subtree_memory(
					node->children.nodes[i]);
		}
		return s;
	}
	if (node->children.offsets != NULL) {
		s += (size_t)(node->key_count + 1) *
		     sizeof(*node->children.offsets);
	}
	return s;
}

static struct vy_page_index_entry
vy_page_index_entry_copy(struct vy_page_index_entry *entry)
{
	struct vy_page_index_entry result = *entry;
	if (entry->min_key != NULL)
		result.min_key = mp_dup(entry->min_key);
	return result;
}

static void
vy_page_index_entry_destroy(struct vy_page_index_entry *entry)
{
	if (entry->min_key != NULL)
		free(entry->min_key);
	entry->idx = 0;
	entry->min_key = NULL;
	entry->min_key_hint = HINT_NONE;
}

static void
vy_page_index_btree_node_destroy(struct vy_page_index_btree_node *node)
{
	if (node->borrowed)
		return;
	if (node->keys != NULL) {
		for (uint32_t i = 0; i < node->key_count; i++)
			vy_page_index_entry_destroy(&node->keys[i]);
		free(node->keys);
	}
	if (node->children_in_memory) {
		if (node->children.nodes != NULL) {
			for (uint32_t i = 0; i < node->key_count + 1; i++) {
				if (node->children.nodes[i] != NULL) {
					vy_page_index_btree_node_destroy(
						node->children.nodes[i]);
					free(node->children.nodes[i]);
				}
			}
			free(node->children.nodes);
		}
	} else {
		free(node->children.offsets);
	}
	TRASH(node);
}

static struct vy_page_index_btree_node
vy_page_index_btree_node_move(struct vy_page_index_btree_node *node)
{
	struct vy_page_index_btree_node result = *node;
	memset(node, 0, sizeof(*node));
	node->borrowed = true;
	return result;
}

MAYBE_UNUSED static struct vy_page_index_btree *
vy_page_index_btree_new(struct key_def *cmp_def, uint64_t root_offset,
			const char *filepath)
{
	/*
	 * TODO: think about where to use xalloc instead of checking result
	 * and setting proper diag.
	 */
	/* TODO: factor out to _new or _create function. */
	struct vy_page_index_btree *btree =
		xcalloc(1, sizeof(struct vy_page_index_btree));
	btree->cmp_def = cmp_def;
	btree->root_offset = root_offset;
	btree->data_offset = UINT64_MAX;
	btree->filepath = (char *)filepath;
	btree->fd = -1;
	return btree;
}

static void
vy_page_index_btree_create(struct vy_page_index_btree *btree,
			   struct key_def *cmp_def,
			   struct vy_page_index_cache_env *env,
			   uint64_t root_offset, uint64_t data_offset,
			   const char *filepath,
			   uint32_t page_count)
{
	btree->cmp_def = key_def_dup(cmp_def);
	btree->env = env;
	btree->root_offset = root_offset;
	btree->data_offset = data_offset;
	btree->filepath = strdup(filepath);
	btree->fd = -1;
	btree->page_count = page_count;
	btree->btree_memory_factor =
		env != NULL ? env->btree_memory_factor : 0.5;
	/* Computed at open from total node memory * btree_memory_factor. */
	btree->in_memory_depth = 0;
	btree->root = NULL;
}

static void
vy_page_index_btree_destroy(struct vy_page_index_btree *btree)
{
	if (btree->root != NULL) {
		if (btree->env != NULL) {
			size_t bytes = vy_page_index_btree_subtree_memory(btree->root);
			assert(btree->env->btree_mem_used >= bytes);
			btree->env->btree_mem_used -= bytes;
		}
		vy_page_index_btree_node_destroy(btree->root);
		free(btree->root);
		btree->root = NULL;
	}
	if (btree->fd >= 0) {
		if (close(btree->fd) < 0)
			say_syserror("close failed");
		btree->fd = -1;
	}
	free(btree->filepath);
	key_def_delete(btree->cmp_def);
	TRASH(btree);
}

static int
vy_page_index_btree_node_read(struct vy_page_index_btree *btree,
			      struct vy_page_index_btree_node *node,
			      uint64_t offset);

static int
vy_page_index_btree_calc_in_memory_depth(struct vy_page_index_btree *btree);

static struct vy_page_index_btree_node *
vy_page_index_btree_read_in_memory(struct vy_page_index_btree *btree,
				   uint64_t node_offset, uint32_t depth);

static int
vy_page_index_btree_open(struct vy_page_index_btree *btree)
{
	if (btree->fd < 0) {
		int fd = open(btree->filepath, O_RDONLY);
		if (fd < 0) {
			diag_set(SystemError, "Can't open path: %s",
				 btree->filepath);
			return -1;
		}
		btree->fd = fd;
		assert(btree->data_offset != UINT64_MAX);
	}
	if (btree->root != NULL)
		return 0;
	if (btree->in_memory_depth == 0) {
		int depth = vy_page_index_btree_calc_in_memory_depth(btree);
		if (depth < 0)
			return -1;
		btree->in_memory_depth = (uint32_t)depth;
	}
	if (btree->in_memory_depth == 0)
		return 0;
	btree->root = vy_page_index_btree_read_in_memory(
		btree, btree->root_offset, 0);
	if (btree->root == NULL)
		return -1;
	if (btree->env != NULL) {
		btree->env->btree_mem_used +=
			vy_page_index_btree_subtree_memory(btree->root);
	}
	return 0;
}

/* {{ B Tree node serialize/deserialize */

static int
vy_page_index_entry_encode(const struct vy_page_index_entry *entry,
			   struct ibuf *wbuf)
{
	const char *min_key_end = entry->min_key;
	assert(mp_typeof(*min_key_end) == MP_ARRAY);
	mp_next(&min_key_end);
	uint32_t min_key_size = min_key_end - entry->min_key;

	uint32_t entry_size = sizeof(int32_t) + min_key_size;
	size_t chunk = sizeof(uint32_t) + entry_size;
	char *pos = ibuf_alloc(wbuf, chunk);
	if (pos == NULL) {
		diag_set(OutOfMemory, chunk, "ibuf_alloc", "page index entry");
		return -1;
	}
	memcpy(pos, &entry_size, sizeof(uint32_t));
	pos += sizeof(uint32_t);
	memcpy(pos, &entry->idx, sizeof(int32_t));
	pos += sizeof(int32_t);
	memcpy(pos, entry->min_key, min_key_size);
	return 0;
}

static int
vy_page_index_entry_decode(struct vy_page_index_entry *entry,
			   const char *pos, uint32_t entry_size,
			   struct key_def *cmp_def,
			   const char *filename)
{
	if (entry_size < sizeof(int32_t) + 1) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 filename, "Invalid entry size");
		return -1;
	}
	const char *end = pos + entry_size;
	memcpy(&entry->idx, pos, sizeof(int32_t));
	pos += sizeof(int32_t);
	if (pos >= end || mp_typeof(*pos) != MP_ARRAY) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 filename, "Invalid entry key");
		return -1;
	}
	const char *key_beg = pos;
	mp_next(&pos);
	if (pos > end) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 filename, "Invalid entry key size");
		return -1;
	}
	entry->min_key = mp_dup(key_beg);
	uint32_t part_count = mp_decode_array(&key_beg);
	entry->min_key_hint = key_hint(key_beg, part_count, cmp_def);
	return 0;
}

static int
vy_page_index_btree_read_meta(const char *filepath, uint64_t *root_offset,
			      uint64_t *data_offset)
{
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, filepath) != 0)
		return -1;
	if (strcmp(cursor.meta.filetype, XLOG_META_TYPE_BTREE) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_BTREE, cursor.meta.filetype);
		goto fail;
	}
	off_t tx_offset = xlog_cursor_pos(&cursor);
	int rc = xlog_cursor_next_tx(&cursor);
	if (rc != 0) {
		if (rc > 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
				 "Unexpected end of file");
		}
		goto fail;
	}
	const char *tx_start = cursor.tx_cursor.rpos;
	const char *tx_pos = tx_start;
	const char *tx_end = cursor.tx_cursor.wpos;
	struct xrow_header xrow;
	if (xrow_decode(&xrow, &tx_pos, tx_end, false) != 0) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
			 "Can't decode btree xrow");
		goto fail;
	}
	if (xrow.type != VY_INDEX_BTREE) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
			 "Invalid btree xrow type");
		goto fail;
	}
	if (tx_pos != tx_end) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
			 "Unexpected extra xrows in btree file");
		goto fail;
	}
	const char *pos = xrow.body->iov_base;
	const char *end = pos + xrow.body->iov_len;
	if (mp_typeof(*pos) != MP_MAP) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
			 "Invalid btree xrow body");
		goto fail;
	}
	uint32_t map_size = mp_decode_map(&pos);
	bool has_data = false;
	for (uint32_t i = 0; i < map_size; i++) {
		uint32_t key = mp_decode_uint(&pos);
		if (key != VY_BTREE_DATA) {
			mp_next(&pos);
			continue;
		}
		uint32_t size = 0;
		const char *data = mp_decode_bin(&pos, &size);
		if (size == 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
				 "Invalid btree payload");
			goto fail;
		}
		*root_offset = 0;
		*data_offset = tx_offset + XLOG_FIXHEADER_SIZE +
			       (uint64_t)(data - tx_start);
		has_data = true;
	}
	if (!has_data || pos != end) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
			 "Invalid btree metadata map");
		goto fail;
	}
	rc = xlog_cursor_next_tx(&cursor);
	if (rc != 1) {
		if (rc == 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filepath,
				 "Unexpected extra transactions in btree file");
		}
		goto fail;
	}
	xlog_cursor_close(&cursor, false);
	return 0;
fail:
	xlog_cursor_close(&cursor, false);
	return -1;
}

static int
vy_page_index_btree_ibuf_ensure(struct ibuf *buf, struct vy_page_index_btree *btree,
				int fd, const char *filename,
				uint64_t *read_offset, size_t need)
{
	enum {
		/*
		 * There is no point in a large read ahead and especially an
		 * read ahead adaptation mechanism. We don't read the entire
		 * file from left to right, but rather jump around it.
		 */
		VY_PAGE_INDEX_BTREE_READ_AHEAD = 4096,
	};
	while (ibuf_used(buf) < need) {
		size_t to_load = MAX(need - ibuf_used(buf),
				     VY_PAGE_INDEX_BTREE_READ_AHEAD);
		void *dst = ibuf_reserve(buf, to_load);
		if (dst == NULL) {
			diag_set(OutOfMemory, to_load, "ibuf_reserve",
				 "btree node read buffer");
			return -1;
		}
		/* TODO: use some fiber-cooperating approach for reading. */
		ssize_t nrd = fio_pread(fd, dst, to_load, *read_offset);
		if (nrd < 0) {
			diag_set(SystemError, "failed to read btree node");
			return -1;
		}
		if (btree != NULL && btree->env != NULL) {
			pm_atomic_fetch_add(&btree->env->io.read_bytes, nrd);
			pm_atomic_fetch_add(&btree->env->io.read_ops, 1);
		}
		if (nrd == 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
				 "Unexpected end of file");
			return -1;
		}
		/*
		 * ibuf_reserve() has been called above,
		 * ibuf_alloc() must not fail.
		 */
		ibuf_alloc(buf, nrd);
		*read_offset += nrd;
	}
	return 0;
}

/**
 * Write a B-tree node to disk.
 * Format:
 * - uint8_t: node type (0 = leaf, 1 = internal)
 * - uint32_t: key_count
 * - For each key: uint32_t entry_size + entry payload
 *   - int32_t idx
 *   - msgpack array min_key
 * - For internal nodes: uint64_t array of child offsets (key_count + 1)
 *
 * @param node Node to write.
 * @param fd File descriptor.
 * @param offset Current file offset (will be updated after write).
 * @param cmp_def Key definition for comparison.
 *
 * @retval 0 Success.
 * @retval -1 Error.
 */
static int
vy_page_index_btree_node_write(struct vy_page_index_btree_node *node,
			       struct ibuf *wbuf, uint64_t *offset,
			       uint64_t *children_offset)
{
	/* Header: node type + key_count. */
	size_t header_size = sizeof(uint8_t) + sizeof(uint32_t);
	char *pos = ibuf_alloc(wbuf, header_size);
	if (pos == NULL) {
		diag_set(OutOfMemory, header_size,
			 "ibuf_alloc", "btree node write buffer");
		return -1;
	}
	memcpy(pos, &node->type, sizeof(uint8_t));
	pos += sizeof(uint8_t);
	memcpy(pos, &node->key_count, sizeof(uint32_t));

	/* Body: all vy_page_index_entry. */
	for (uint32_t i = 0; i < node->key_count; i++)
		if (vy_page_index_entry_encode(&node->keys[i], wbuf) != 0)
			return -1;

	/* Children offsets (internal nodes only). */
	if (node->type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		assert(node->children.offsets != NULL);
		assert(children_offset != NULL);
		/*
		 * children_offset is an offset within the write buffer, not the
		 * resulting file offset. It is used by vy_page_index_btree_node_patch_children()
		 * which patches wbuf->rpos directly.
		 */
		*children_offset = (uint64_t)ibuf_used(wbuf);

		size_t child_count = (size_t)node->key_count + 1;
		pos = ibuf_alloc(wbuf, child_count * sizeof(uint64_t));
		if (pos == NULL) {
			diag_set(OutOfMemory, child_count * sizeof(uint64_t),
				 "ibuf_alloc", "btree node write buffer");
			return -1;
		}
		memcpy(pos, node->children.offsets, child_count * sizeof(uint64_t));
	}

	size_t size = ibuf_used(wbuf) - (size_t)*offset;
	*offset += size;
	return 0;
}

static int
vy_page_index_btree_node_patch_children(struct ibuf *wbuf,
					uint64_t children_offset,
					const uint64_t *children, uint32_t n)
{
	assert(children_offset + sizeof(*children) * n <=
	       (uint64_t)ibuf_used(wbuf));
	memcpy(wbuf->rpos + children_offset, children, sizeof(*children) * n);
	return 0;
}

/** Read a B-tree node from disk. */
static int
vy_page_index_btree_node_read(struct vy_page_index_btree *btree,
			      struct vy_page_index_btree_node *node,
			      uint64_t offset)
{
	memset(node, 0, sizeof(*node));
	node->offset = offset;

	if (btree->fd < 0) {
		diag_set(SystemError, "B-tree file is not opened: %s",
			 btree->filepath);
		return -1;
	}

	struct ibuf rbuf;
	ibuf_create(&rbuf, &cord()->slabc, 1024);
	uint64_t read_offset = btree->data_offset + offset;

	size_t bytes = sizeof(uint8_t) + sizeof(uint32_t);

	/* Header: type + key_count */
	if (vy_page_index_btree_ibuf_ensure(
	    &rbuf, btree, btree->fd, btree->filepath, &read_offset, bytes) != 0)
		goto fail;

	node->type = (enum vy_page_index_btree_node_type)
		*(const uint8_t *)rbuf.rpos;
	rbuf.rpos += sizeof(uint8_t);

	if (node->type != VY_PAGE_INDEX_BTREE_NODE_LEAF &&
	    node->type != VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, btree->filepath,
			 "Invalid node type");
		goto fail;
	}

	node->key_count = *(const uint32_t *)rbuf.rpos;
	rbuf.rpos += sizeof(uint32_t);

	if (node->key_count == 0 ||
	    node->key_count > VY_PAGE_INDEX_BTREE_ORDER) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, btree->filepath,
			 "Invalid key count");
		goto fail;
	}

	bytes = node->key_count * sizeof(*node->keys);
	node->keys = calloc(node->key_count, sizeof(*node->keys));
	if (node->keys == NULL) {
		diag_set(OutOfMemory, bytes, "malloc", "btree node keys");
		goto fail;
	}

	for (uint32_t i = 0; i < node->key_count; i++) {
		/* Read entry size prefix. */
		if (vy_page_index_btree_ibuf_ensure(
		    &rbuf, btree, btree->fd, btree->filepath, &read_offset,
		    sizeof(uint32_t)) != 0)
			goto fail;
		uint32_t entry_size = *(const uint32_t *)rbuf.rpos;
		rbuf.rpos += sizeof(uint32_t);
		/* Invalid entry size. */
		if (entry_size == 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 btree->filepath,
				 "Invalid page index entry size");
			goto fail;
		}
		/* Read entry size bytes and decode the entry. */
		if (vy_page_index_btree_ibuf_ensure(
		    &rbuf, btree, btree->fd, btree->filepath, &read_offset,
		    entry_size) != 0)
			goto fail;

		if (vy_page_index_entry_decode(
		    &node->keys[i], rbuf.rpos, entry_size,
		    btree->cmp_def, btree->filepath) != 0)
			goto fail;
		rbuf.rpos += entry_size;
	}

	if (node->type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
		ibuf_destroy(&rbuf);
		return 0;
	}

	/* Read children offsets. */
	size_t child_count = (size_t)node->key_count + 1;
	bytes = child_count * sizeof(uint64_t);
	if (vy_page_index_btree_ibuf_ensure(
	    &rbuf, btree, btree->fd, btree->filepath, &read_offset, bytes) != 0)
		goto fail;
	node->children.offsets = calloc(child_count, sizeof(uint64_t));
	if (node->children.offsets == NULL) {
		diag_set(OutOfMemory, bytes, "calloc", "btree node children");
		goto fail;
	}
	node->children_in_memory = false;
	memcpy(node->children.offsets, rbuf.rpos, bytes);
	rbuf.rpos += bytes;

	ibuf_destroy(&rbuf);
	return 0;

fail:
	ibuf_destroy(&rbuf);
	vy_page_index_btree_node_destroy(node);
	return -1;
}

/** Sum in-memory footprint of all nodes in the subtree at @a offset. */
static int
vy_page_index_btree_measure_subtree_memory(struct vy_page_index_btree *btree,
					   uint64_t offset, size_t *total)
{
	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_read(btree, &node, offset) != 0)
		return -1;
	*total += vy_page_index_btree_node_footprint(&node);
	if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		uint32_t child_count = node.key_count + 1;
		*total += (size_t)child_count * sizeof(*node.children.offsets);
		for (uint32_t i = 0; i < child_count; i++) {
			if (vy_page_index_btree_measure_subtree_memory(
			    btree, node.children.offsets[i], total) != 0) {
				vy_page_index_btree_node_destroy(&node);
				return -1;
			}
		}
	}
	vy_page_index_btree_node_destroy(&node);
	return 0;
}

/**
 * Pick how many upper levels to cache so that their total footprint
 * is at most btree_memory_factor of the full tree size.
 */
static int
vy_page_index_btree_calc_in_memory_depth(struct vy_page_index_btree *btree)
{
	if (btree->btree_memory_factor <= 0.0)
		return 0;

	size_t total = 0;
	if (vy_page_index_btree_measure_subtree_memory(
	    btree, btree->root_offset, &total) != 0)
		return -1;
	if (total == 0)
		return 0;

	double factor = btree->btree_memory_factor;
	if (factor > 1.0)
		factor = 1.0;
	size_t limit = (size_t)((double)total * factor);
	if (limit == 0)
		return 0;

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	uint64_t *frontier = (uint64_t *)region_alloc(region, sizeof(uint64_t));
	if (frontier == NULL) {
		diag_set(OutOfMemory, sizeof(uint64_t),
			 "region_alloc", "btree frontier");
		return -1;
	}
	frontier[0] = btree->root_offset;
	size_t frontier_size = 1;
	uint32_t depth = 0;
	size_t accumulated = 0;

	while (frontier_size > 0 &&
	       depth < VY_PAGE_INDEX_BTREE_MAX_DEPTH) {
		size_t level_mem = 0;
		size_t next_cap = frontier_size *
				  (VY_PAGE_INDEX_BTREE_ORDER + 1);
		uint64_t *next = (uint64_t *)region_alloc(region,
			next_cap * sizeof(*next));
		if (next == NULL) {
			diag_set(OutOfMemory, next_cap * sizeof(*next),
				 "region_alloc", "btree frontier");
			return -1;
		}
		size_t next_size = 0;

		for (size_t i = 0; i < frontier_size; i++) {
			struct vy_page_index_btree_node node;
			if (vy_page_index_btree_node_read(btree, &node,
							  frontier[i]) != 0)
				goto fail;
			level_mem += vy_page_index_btree_node_footprint(&node);
			if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
				uint32_t child_count = node.key_count + 1;
				level_mem += (size_t)child_count *
					       sizeof(*node.children.offsets);
				for (uint32_t c = 0; c < child_count; c++) {
					next[next_size++] =
						node.children.offsets[c];
				}
			}
			vy_page_index_btree_node_destroy(&node);
		}

		if (accumulated + level_mem > limit && depth > 0)
			break;
		accumulated += level_mem;
		depth++;
		frontier = next;
		frontier_size = next_size;
	}

	region_truncate(region, region_svp);
	return (int)depth;
fail:
	region_truncate(region, region_svp);
	return -1;
}

/**
 * Read a B-tree node into memory.
 * A B-tree can store the first few levels in memory.
 *
 * @param btree B-tree.
 * @param node_offset Node offset.
 * @param depth Depth.
 * @return Node (NULL on error).
 */
static struct vy_page_index_btree_node *
vy_page_index_btree_read_in_memory(struct vy_page_index_btree *btree,
				   uint64_t node_offset, uint32_t depth)
{
	/* TODO: use malloc instead of calloc. */
	struct vy_page_index_btree_node *node =
		calloc(1, sizeof(struct vy_page_index_btree_node));
	if (node == NULL) {
		diag_set(OutOfMemory, sizeof(struct vy_page_index_btree_node),
			 "calloc", "btree node");
		return NULL;
	}
	if (vy_page_index_btree_node_read(btree, node, node_offset) != 0) {
		free(node);
		return NULL;
	}
	/* We load into memory no more than in_memory_depth levels. */
	if (node->type == VY_PAGE_INDEX_BTREE_NODE_LEAF ||
	    depth + 1 >= btree->in_memory_depth)
		return node;
	/* Load children into memory. */
	node->children_in_memory = true;
	uint64_t *offsets = node->children.offsets;
	uint32_t child_count = node->key_count + 1;
	node->children.nodes = calloc(child_count,
				      sizeof(*node->children.nodes));
	if (node->children.nodes == NULL) {
		diag_set(OutOfMemory, child_count,
			 "calloc", "btree node children");
		free(node);
		node = NULL;
		goto exit;
	}
	for (uint32_t i = 0; i < node->key_count + 1; i++) {
		/* Load children recursively. */
		node->children.nodes[i] =
			vy_page_index_btree_read_in_memory(
				btree, offsets[i], depth + 1);
		if (node->children.nodes[i] == NULL) {
			vy_page_index_btree_node_destroy(node);
			free(node);
			node = NULL;
			break;
		}
	}
exit:
	/**
	 * Offsets were allocated in vy_page_index_btree_node_read
	 * using calloc.
	 */
	free(offsets);
	return node;
}

/**
 * Get the root node. If the root is cached in memory, @a result
 * receives a shallow (borrowed) copy. Otherwise the node is read
 * from disk into @a result.
 */
static int
vy_page_index_btree_node_get_root(
	struct vy_page_index_btree *btree,
	struct vy_page_index_btree_node *result)
{
	if (btree->root != NULL) {
		*result = *btree->root;
		result->borrowed = true;
		return 0;
	}
	/* The tree is entirely on disk. */
	return vy_page_index_btree_node_read(
		btree, result, btree->root_offset);
}

/**
 * Get the child of @a node at position @a child. If the child is
 * cached in memory, @a result receives a shallow (borrowed) copy.
 * Otherwise the child is read from disk into @a result.
 */
static int
vy_page_index_btree_node_get_child(
	struct vy_page_index_btree *btree,
	struct vy_page_index_btree_node *node, uint32_t child,
	struct vy_page_index_btree_node *result)
{
	assert(node->type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL);
	assert(child <= node->key_count);
	if (node->children_in_memory) {
		*result = *node->children.nodes[child];
		result->borrowed = true;
		return 0;
	}
	/* The current node is deeper than in_memory_depth. */
	uint64_t child_offset = node->children.offsets[child];
	return vy_page_index_btree_node_read(btree, result, child_offset);
}

/* }} B Tree node serialize/deserialize */

/* {{ B Tree iterator */

static inline struct vy_page_index_btree_iterator
vy_page_index_btree_invalid_iterator(void)
{
	return (struct vy_page_index_btree_iterator){
		.node_offset = UINT64_MAX,
		.pos = 0,
		.has_current_node = false,
		.depth = 0,
	};
}

static inline bool
vy_page_index_btree_iterator_is_invalid(const struct vy_page_index_btree_iterator *it)
{
	return it->node_offset == UINT64_MAX;
}

static inline void
vy_page_index_btree_iterator_assert_current_node(
	const struct vy_page_index_btree_iterator *it)
{
	assert(!vy_page_index_btree_iterator_is_invalid(it));
	assert(it->has_current_node);
	assert(it->current_node.offset == it->node_offset);
	(void)it;
}

static void
vy_page_index_btree_iterator_clear_current_node(
	struct vy_page_index_btree_iterator *it)
{
	if (it->has_current_node) {
		vy_page_index_btree_node_destroy(&it->current_node);
		it->has_current_node = false;
	}
}

static void
vy_page_index_btree_iterator_clear_path(
	struct vy_page_index_btree_iterator *it)
{
	for (uint32_t i = 0; i < it->depth; i++)
		vy_page_index_btree_node_destroy(&it->path[i].node);
	it->depth = 0;
}

/* Destroy the iterator. It also invalidates the iterator. */
static void
vy_page_index_btree_iterator_destroy(struct vy_page_index_btree_iterator *it)
{
	vy_page_index_btree_iterator_clear_current_node(it);
	vy_page_index_btree_iterator_clear_path(it);
	*it = vy_page_index_btree_invalid_iterator();
}

/** Detach the cached current node for next/prev (ownership moves to @a node). */
static void
vy_page_index_btree_iterator_take_node(struct vy_page_index_btree_iterator *it,
				       struct vy_page_index_btree_node *node)
{
	vy_page_index_btree_iterator_assert_current_node(it);
	*node = vy_page_index_btree_node_move(&it->current_node);
	it->has_current_node = false;
}

/** Load the current node from disk if it is not cached yet. */
/*static int
vy_page_index_btree_iterator_ensure_current_node(
	struct vy_page_index_btree *btree,
	struct vy_page_index_btree_iterator *it)
{
	if (it->has_current_node)
		return 0;
	if (vy_page_index_btree_node_read(btree, &it->current_node,
					  it->node_offset) != 0) {
		vy_page_index_btree_iterator_destroy(it);
		return -1;
	}
	it->has_current_node = true;
	return 0;
}*/

static int
vy_page_index_btree_iterator_get_elem(struct vy_page_index_btree *btree,
				      struct vy_page_index_btree_iterator *it,
				      struct vy_page_index_entry *result)
{
	if (vy_page_index_btree_iterator_is_invalid(it)) {
		result->idx = INT32_MAX;
		result->min_key = NULL;
		result->min_key_hint = HINT_NONE;
		return 0;
	}
	/* Use the cached node when available, otherwise read from disk. */
	if (!it->has_current_node) {
		struct vy_page_index_btree_node node;
		if (vy_page_index_btree_node_read(btree, &node, it->node_offset) != 0)
			return -1;
		assert(it->pos < node.key_count);
		*result = vy_page_index_entry_copy(&node.keys[it->pos]);
		vy_page_index_btree_node_destroy(&node);
		return 0;
	}
	vy_page_index_btree_iterator_assert_current_node(it);
	assert(it->pos < it->current_node.key_count);
	*result = vy_page_index_entry_copy(&it->current_node.keys[it->pos]);
	return 0;
}

static inline void
vy_page_index_invalid_diag_set(const char *filename, const char *message)
{
	diag_set(ClientError, ER_INVALID_INDEX_FILE, filename, message);
}

/* Push a path entry to the iterator. Takes ownership of @a node. */
static inline int
vy_page_index_btree_path_push(struct vy_page_index_btree *btree,
			      struct vy_page_index_btree_iterator *it,
			      struct vy_page_index_btree_node *node,
			      uint32_t child)
{
	if (it->depth >= VY_PAGE_INDEX_BTREE_MAX_DEPTH) {
		vy_page_index_invalid_diag_set(
			btree->filepath, "B-tree depth limit exceeded");
		return -1;
	}
	it->path[it->depth].node = vy_page_index_btree_node_move(node);
	it->path[it->depth].child = child;
	++it->depth;
	return 0;
}

/**
 * Descent to the left (to the minimum) until a leaf (Finding the minimum in a
 * subtree). Takes ownership of @a start.
 */
static int
vy_page_index_btree_iterator_descend_min(struct vy_page_index_btree *btree,
					 struct vy_page_index_btree_iterator *it,
					 struct vy_page_index_btree_node *start)
{
	struct vy_page_index_btree_node node = *start;
	while (true) {
		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			vy_page_index_btree_iterator_clear_current_node(it);
			it->node_offset = node.offset;
			it->pos = 0;
			it->current_node = vy_page_index_btree_node_move(&node);
			it->has_current_node = true;
			return 0;
		}
		uint32_t child = 0;
		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			return -1;
		}
		/* The parent is moved into the path and not used anymore. */
		if (vy_page_index_btree_path_push(btree, it, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			return -1;
		}
		node = child_node;
	}
}

/**
 * Descent to the right (to the maximum) until a leaf (Finding the maximum in a
 * subtree). Takes ownership of @a start.
 */
static int
vy_page_index_btree_iterator_descend_max(struct vy_page_index_btree *btree,
				         struct vy_page_index_btree_iterator *it,
				         struct vy_page_index_btree_node *start)
{
	struct vy_page_index_btree_node node = *start;
	while (true) {
		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			vy_page_index_btree_iterator_clear_current_node(it);
			it->node_offset = node.offset;
			it->pos = node.key_count - 1;
			it->current_node = vy_page_index_btree_node_move(&node);
			it->has_current_node = true;
			return 0;
		}
		uint32_t child = node.key_count;
		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			return -1;
		}
		/* The parent is moved into the path and not used anymore. */
		if (vy_page_index_btree_path_push(btree, it, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			return -1;
		}
		node = child_node;
	}
}

static int MAYBE_UNUSED
vy_page_index_btree_iterator_next(struct vy_page_index_btree *btree,
				  struct vy_page_index_btree_iterator *it)
{
	if (vy_page_index_btree_iterator_is_invalid(it))
		return 0;

	vy_page_index_btree_iterator_assert_current_node(it);
	//if (vy_page_index_btree_iterator_ensure_current_node(btree, it) != 0)
	//	return -1;

	struct vy_page_index_btree_node node = it->current_node;

	if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		/* This node is not more needed for iterator, so we can move it. */
		vy_page_index_btree_iterator_take_node(it, &node);
		/* Go down to the right sibling subtree. */
		uint32_t child = it->pos + 1;
		assert(child <= node.key_count);
		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		if (vy_page_index_btree_path_push(btree, it, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		/* Descent left to the minimum >= current. */
		if (vy_page_index_btree_iterator_descend_min(
		    btree, it, &child_node) != 0) {
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		return 0;
	}

	/* Leaf. */
	uint32_t key_count = node.key_count;
	if (it->pos + 1 < key_count) {
		++it->pos;
		return 0;
	}

	/* This node is not more needed for iterator. */
	vy_page_index_btree_iterator_clear_current_node(it);

	/* Ascend. */
	while (it->depth > 0) {
		struct vy_page_index_btree_path_entry *entry =
			&it->path[--it->depth];
		if (entry->child < entry->node.key_count) {
			it->node_offset = entry->node.offset;
			it->pos = entry->child;
			it->current_node = vy_page_index_btree_node_move(
				&entry->node);
			it->has_current_node = true;
			return 0;
		}
		vy_page_index_btree_node_destroy(&entry->node);
	}
	vy_page_index_btree_iterator_destroy(it);
	return 0;
}

static int
vy_page_index_btree_iterator_prev(struct vy_page_index_btree *btree,
				  struct vy_page_index_btree_iterator *it)
{
	if (vy_page_index_btree_iterator_is_invalid(it))
		return 0;

	vy_page_index_btree_iterator_assert_current_node(it);
	//if (vy_page_index_btree_iterator_ensure_current_node(btree, it) != 0)
	//	return -1;

	struct vy_page_index_btree_node node = it->current_node;

	if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		/* This node is not more needed for iterator, so we can move it. */
		vy_page_index_btree_iterator_take_node(it, &node);
		/* Go down to the left sibling subtree. */
		uint32_t child = it->pos;
		assert(child <= node.key_count);
		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		if (vy_page_index_btree_path_push(btree, it, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		/* Descent right to the maximum <= current. */
		if (vy_page_index_btree_iterator_descend_max(
		    btree, it, &child_node) != 0) {
			vy_page_index_btree_iterator_destroy(it);
			return -1;
		}
		return 0;
	}

	/* Leaf. */
	if (it->pos > 0) {
		--it->pos;
		return 0;
	}

	/* This node is not more needed for iterator. */
	vy_page_index_btree_iterator_clear_current_node(it);

	/* Ascend. */
	while (it->depth > 0) {
		struct vy_page_index_btree_path_entry *entry =
			&it->path[--it->depth];
		if (entry->child > 0) {
			it->node_offset = entry->node.offset;
			it->pos = entry->child - 1;
			it->current_node = vy_page_index_btree_node_move(
				&entry->node);
			it->has_current_node = true;
			return 0;
		}
		vy_page_index_btree_node_destroy(&entry->node);
	}
	vy_page_index_btree_iterator_destroy(it);
	return 0;
}

/* }} B Tree iterator */

/* {{ B Tree lower/upper_bound */

/* Binary search in keys[] for the lower bound. */
static uint32_t
vy_page_index_btree_node_lower_bound(struct vy_page_index_btree *btree,
				     struct vy_page_index_btree_node *node,
				     struct vy_entry key,
				     bool *equal_key)
{
	*equal_key = false;
	assert(node->key_count > 0);
	int32_t range[2] = { -1, (int32_t)node->key_count };
	do {
		int32_t mid = range[0] + (range[1] - range[0]) / 2;
		assert(mid >= 0 && mid < (int32_t)node->key_count);
		int cmp = vy_page_index_entry_compare_with_key(
			&node->keys[mid], key, btree->cmp_def);
		range[cmp <= 0] = mid;
		*equal_key = *equal_key || cmp == 0;
	} while (range[1] - range[0] > 1);
	return range[1];
}

/* Binary search in keys[] for the upper bound. */
static uint32_t
vy_page_index_btree_node_upper_bound(struct vy_page_index_btree *btree,
				     struct vy_page_index_btree_node *node,
				     struct vy_entry key,
				     bool *equal_key)
{
	*equal_key = false;
	assert(node->key_count > 0);
	int32_t range[2] = { -1, (int32_t)node->key_count };
	do {
		int32_t mid = range[0] + (range[1] - range[0]) / 2;
		assert(mid >= 0 && mid < (int32_t)node->key_count);
		int cmp = vy_page_index_entry_compare_with_key(
			&node->keys[mid], key, btree->cmp_def);
		/* The only difference is we are using < instead of <=. */
		range[cmp < 0] = mid;
		*equal_key = *equal_key || cmp == 0;
	} while (range[1] - range[0] > 1);
	return range[1];
}

static int
vy_page_index_btree_lower_bound(struct vy_page_index_btree *btree,
				struct vy_entry key,
				struct vy_page_index_btree_iterator *result,
				bool *equal_key)
{
	*equal_key = false;
	*result = vy_page_index_btree_invalid_iterator();

	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_get_root(btree, &node) != 0) {
		*result = vy_page_index_btree_invalid_iterator();
		return -1;
	}

	int64_t found_depth = -1;

	/* Descend. */
	while (true) {
		bool eq = false;
		uint32_t child = vy_page_index_btree_node_lower_bound(
			btree, &node, key, &eq);
		*equal_key = *equal_key || eq;

		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			if (child < node.key_count) {
				result->node_offset = node.offset;
				result->pos = child;
				result->has_current_node = true;
				result->current_node =
					vy_page_index_btree_node_move(&node);
			} else {
				/* All keys in the leaf are < @a key. */
				if (found_depth != -1) {
					result->has_current_node = true;
					result->current_node = result->path[found_depth].node;
					for (int i = result->depth; i < found_depth; i++)
						vy_page_index_btree_node_destroy(&result->path[i].node);
					result->depth = found_depth;
				}
				vy_page_index_btree_node_destroy(&node);
			}
			return 0;
		}

		if (child < node.key_count) {
			result->node_offset = node.offset;
			result->pos = child;
			found_depth = result->depth;
		}

		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			goto fail;
		}
		if (vy_page_index_btree_path_push(btree, result, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			goto fail;
		}
		node = child_node;
	}
fail:
	vy_page_index_btree_iterator_destroy(result);
	return -1;
}

static int
vy_page_index_btree_upper_bound(struct vy_page_index_btree *btree,
				struct vy_entry key,
				struct vy_page_index_btree_iterator *result,
				bool *equal_key)
{
	*equal_key = false;
	*result = vy_page_index_btree_invalid_iterator();

	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_get_root(btree, &node) != 0) {
		*result = vy_page_index_btree_invalid_iterator();
		return -1;
	}

	int64_t found_depth = -1;

	/* Descend. */
	while (true) {
		bool eq = false;
		uint32_t child = vy_page_index_btree_node_upper_bound(
			btree, &node, key, &eq);
		*equal_key = *equal_key || eq;

		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			if (child < node.key_count) {
				result->node_offset = node.offset;
				result->pos = child;
				result->has_current_node = true;
				result->current_node =
					vy_page_index_btree_node_move(&node);
			} else {
				/* All keys in the leaf are > @a key. */
				if (found_depth != -1) {
					result->has_current_node = true;
					result->current_node = result->path[found_depth].node;
					for (int i = result->depth; i < found_depth; i++)
						vy_page_index_btree_node_destroy(&result->path[i].node);
					result->depth = found_depth;
				}
				vy_page_index_btree_node_destroy(&node);
			}
			return 0;
		}

		if (child < node.key_count) {
			result->node_offset = node.offset;
			result->pos = child;
			found_depth = result->depth;
		}

		struct vy_page_index_btree_node child_node;
		if (vy_page_index_btree_node_get_child(
		    btree, &node, child, &child_node) != 0) {
			vy_page_index_btree_node_destroy(&node);
			goto fail;
		}
		if (vy_page_index_btree_path_push(btree, result, &node, child) != 0) {
			vy_page_index_btree_node_destroy(&node);
			vy_page_index_btree_node_destroy(&child_node);
			goto fail;
		}
		node = child_node;
	}
fail:
	vy_page_index_btree_iterator_destroy(result);
	return -1;
}

/* Find the last element in the tree. */
static int
vy_page_index_btree_last(struct vy_page_index_btree *btree,
			 struct vy_page_index_btree_iterator *it)
{
	memset(it, 0, sizeof(*it));
	struct vy_page_index_btree_node root;
	if (vy_page_index_btree_node_get_root(btree, &root) != 0)
		return -1;
	return vy_page_index_btree_iterator_descend_max(btree, it, &root);
}

/**
 * Find the chain of elements for the given key.
 * A chain is a pair of consecutive elements.
 * The B-tree must be opened by the caller before this function is used.
 */
static int
vy_page_index_btree_find_chain(struct vy_page_index_btree *btree,
			       struct vy_entry key, bool lower_bound,
			       struct vy_page_index_entry *next,
			       struct vy_page_index_entry *prev,
			       bool *equal_key)
{
	assert(btree->fd >= 0);
	assert(btree->in_memory_depth == 0 || btree->root != NULL);

	struct vy_page_index_btree_iterator it;
	if (lower_bound) {
		if (vy_page_index_btree_lower_bound(btree, key, &it,
						    equal_key) != 0)
			goto fail;
	} else {
		if (vy_page_index_btree_upper_bound(btree, key, &it,
						    equal_key) != 0)
			goto fail;
	}

	if (vy_page_index_btree_iterator_get_elem(btree, &it, next) != 0)
		goto fail;
	if (next->idx == INT32_MAX) {
		next->idx = btree->page_count;
		next->min_key = NULL;
		next->min_key_hint = HINT_NONE;
		vy_page_index_btree_iterator_destroy(&it);
		if (vy_page_index_btree_last(btree, &it) != 0)
			goto fail;
	} else {
		if (next->idx == 0) {
			prev->idx = -1;
			prev->min_key = NULL;
			prev->min_key_hint = HINT_NONE;
			goto out;
		}
		if (vy_page_index_btree_iterator_prev(btree, &it) != 0)
			goto fail;
	}
	if (vy_page_index_btree_iterator_get_elem(btree, &it, prev) != 0)
		goto fail;

out:
	assert(prev->idx != INT32_MAX);
	vy_page_index_btree_iterator_destroy(&it);
	return 0;
fail:
	vy_page_index_btree_iterator_destroy(&it);
	return -1;
}

/* }} B Tree lower/upper_bound */

/* {{ B Tree build */

/**
 * Build a B-tree from vy_page_index_entry range [lo, hi) sorted in ascending
 * order by min key.
 *
 * Internal nodes store separator keys taken from the source array (i.e. keys
 * are not duplicated between levels like in a B+ tree).
 *
 * @return file offset of the written node or -1 on error.
 */
static int
vy_page_index_btree_build_range(struct vy_page_index_entry *pages,
				uint32_t lo, uint32_t hi,
				struct ibuf *wbuf, uint64_t *offset,
				uint64_t *node_offset)
{
	assert(lo <= hi);
	const uint32_t n = hi - lo;

	assert(n > 0);

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	struct vy_page_index_btree_node node;
	memset(&node, 0, sizeof(node));

	/* Leaf. */
	if (n <= VY_PAGE_INDEX_BTREE_ORDER) {
		node.type = VY_PAGE_INDEX_BTREE_NODE_LEAF;
		node.key_count = n;
		size_t bytes = (size_t)n * sizeof(*node.keys);
		node.keys = region_alloc(region, bytes);
		if (node.keys == NULL) {
			diag_set(OutOfMemory, bytes, "region_alloc",
				 "btree leaf keys");
			/* Nothing to free. */
			assert(region_svp == region_used(region));
			return -1;
		}
		memset(node.keys, 0, bytes);
		for (uint32_t i = 0; i < n; i++) {
			node.keys[i] = pages[lo + i];
		}
		*node_offset = *offset;
		int rc = vy_page_index_btree_node_write(
			&node, wbuf, offset, NULL);
		region_truncate(region, region_svp);
		return rc;
	}

	/* Internal. */
	node.type = VY_PAGE_INDEX_BTREE_NODE_INTERNAL;
	/*
	 * Pick key_count separator keys so that (n - key_count) keys can be
	 * split into (key_count + 1) non-empty children:
	 * 	(n - key_count) >= (key_count + 1)
	 * 	n >= 2key_count + 1
	 * 	key_count <= (n - 1) / 2
	 * Also k is upper bounded by ORDER.
	 */
	node.key_count = MIN((n - 1) / 2, VY_PAGE_INDEX_BTREE_ORDER);
	assert(node.key_count >= 1);

	const uint32_t children_count = node.key_count + 1;
	const uint32_t remaining = n - node.key_count;
	assert(remaining >= children_count);
	
	size_t keys_bytes = (size_t)node.key_count * sizeof(*node.keys);
	node.keys = region_alloc(region, keys_bytes);
	if (node.keys == NULL) {
		/* Nothing to free. */
		assert(region_svp == region_used(region));
		return -1;
	}
	memset(node.keys, 0, keys_bytes);
	/*
	 * Write the internal node first with zeroed children offsets,
	 * then build children and patch offsets.
	 */
	uint64_t zero_children[VY_PAGE_INDEX_BTREE_ORDER + 1] = {0};
	node.children.offsets = zero_children;

	const uint32_t base = remaining / children_count;
	/*
	 * The first `extra` pages will have one more record than other
	 * `remaining - extra`.
	 */
	const uint32_t extra = remaining % children_count;
	assert(base >= 1);

	/* Precompute child ranges. */
	uint32_t children_lo[VY_PAGE_INDEX_BTREE_ORDER + 1];
	uint32_t children_hi[VY_PAGE_INDEX_BTREE_ORDER + 1];

	uint32_t pos = lo;
	for (uint32_t i = 0; i < children_count; i++) {
		assert(pos < hi);
		const uint32_t child_keys = base + (i < extra);
		children_lo[i] = pos;
		children_hi[i] = pos + child_keys;
		pos = children_hi[i];

		/* Separator after each child except the last. */
		if (i < node.key_count) {
			assert(pos < hi);
			node.keys[i] = pages[pos];
			pos++;
		}
	}
	assert(pos == hi);

	/*
	 * Write the internal node first with zeroed child offsets, then build
	 * children and patch offsets in-place. This makes upper levels closer
	 * to the beginning of the file.
	 * TODO: Due to this, it is possible to first write all nodes in a
	 * buffered manner, and then patch the children links in one pass.
	 */
	*node_offset = *offset;
	uint64_t children_offset = 0;
	int rc = vy_page_index_btree_node_write(&node, wbuf, offset,
						&children_offset);
	region_truncate(region, region_svp);

	if (rc != 0)
		return -1;

	uint64_t children[VY_PAGE_INDEX_BTREE_ORDER + 1];
	for (uint32_t i = 0; i < children_count; i++) {
		if (vy_page_index_btree_build_range(
		    pages, children_lo[i], children_hi[i],
		    wbuf, offset, &children[i]) != 0) {
			return -1;
		}
	}
	if (vy_page_index_btree_node_patch_children(
	    wbuf, children_offset, children, children_count) != 0)
		return -1;
	return 0;
}

/**
 * Build B-tree from sorted array of vy_page_index_entry and write to disk.
 *
 * @param pages Sorted array of vy_page_index_entry (sorted by min_key).
 * @param page_count Number of pages.
 * @param fd File descriptor to write to.
 * @param start_offset Starting offset in file (will be updated).
 * @param cmp_def Key definition for comparison.
 * @param[out] root_offset Offset of root node in file.
 *
 * @retval 0 Success.
 * @retval -1 Error.
 */
static int
vy_page_index_btree_build(struct vy_page_index_entry *pages, uint32_t page_count,
			  const char *filepath)
{
	assert(page_count > 0);
	struct ibuf wbuf;
	ibuf_create(&wbuf, &cord()->slabc, 4096);

	uint64_t root_offset = 0;
	uint64_t written = 0;
	if (vy_page_index_btree_build_range(pages, 0, page_count,
					    &wbuf, &written, &root_offset) != 0)
		goto fail;

	struct xlog xlog;
	struct xlog_meta meta;
	xlog_meta_create(&meta, XLOG_META_TYPE_BTREE, &INSTANCE_UUID,
			 NULL, NULL);
	struct xlog_opts opts = xlog_opts_default;
	opts.no_compression = true;
	if (xlog_create(&xlog, filepath, 0, &meta, &opts) != 0)
		goto fail;

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	size_t body_size = mp_sizeof_map(1) +
			   mp_sizeof_uint(VY_BTREE_DATA) +
			   mp_sizeof_bin(ibuf_used(&wbuf));
	char *pos = region_alloc(region, body_size);
	if (pos == NULL) {
		diag_set(OutOfMemory, body_size, "region", "btree xrow");
		goto fail_discard_xlog;
	}
	struct xrow_header xrow;
	memset(&xrow, 0, sizeof(xrow));
	xrow.type = VY_INDEX_BTREE;
	xrow.body->iov_base = pos;
	pos = mp_encode_map(pos, 1);
	pos = mp_encode_uint(pos, VY_BTREE_DATA);
	pos = mp_encode_binl(pos, ibuf_used(&wbuf));
	memcpy(pos, wbuf.rpos, ibuf_used(&wbuf));
	pos += ibuf_used(&wbuf);
	xrow.body->iov_len = pos - (char *)xrow.body->iov_base;
	xrow.bodycnt = 1;

	xlog_tx_begin(&xlog);
	if (xlog_write_row(&xlog, &xrow) < 0) {
		region_truncate(region, region_svp);
		goto fail_discard_xlog;
	}
	region_truncate(region, region_svp);
	if (xlog_tx_commit(&xlog) < 0)
		goto fail_discard_xlog;
	if (xlog_close(&xlog) != 0 || xlog_materialize(&xlog) != 0)
		goto fail_discard_xlog;
	ibuf_destroy(&wbuf);
	return 0;

fail_discard_xlog:
	xlog_discard(&xlog);
fail:
	ibuf_destroy(&wbuf);
	return -1;
}

/* Write page index B-tree to file. */
static int
vy_page_index_btree_write(struct vy_page_index_entry *entries,
			  uint32_t page_count,
			  const char *filepath,
			  uint64_t *root_offset,
			  uint64_t *data_offset,
			  struct vy_page_index_cache_env *env)
{
	if (vy_page_index_btree_build(entries, page_count, filepath) != 0)
		return -1;
	if (vy_page_index_btree_read_meta(filepath,
					  root_offset, data_offset) != 0)
		return -1;
	/* Update IO stats. */
	if (env != NULL) {
		struct stat st;
		if (stat(filepath, &st) == 0) {
			env->io.write_bytes += st.st_size;
		}
	}
	return 0;
}

/* }} B Tree build */

/* }}} B Tree */

/* {{{ Page Index Cache - a cache of page index B-tree nodes. */

static void *
vy_page_index_cache_tree_page_alloc(struct matras_allocator *allocator)
{
	struct vy_page_index_cache_env *env =
		container_of(allocator, struct vy_page_index_cache_env, allocator);
	env->tree_mem_used += VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE;
	return xmalloc(VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE);
}

static void
vy_page_index_cache_tree_page_free(struct matras_allocator *allocator, void *ptr)
{
	struct vy_page_index_cache_env *env =
		container_of(allocator, struct vy_page_index_cache_env, allocator);
	assert(env->tree_mem_used >= VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE);
	env->tree_mem_used -= VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE;
	free(ptr);
}

void
vy_page_index_cache_env_create(struct vy_page_index_cache_env *env,
			       struct slab_cache *slab_cache)
{
	rlist_create(&env->cache_lru);
	env->tree_mem_used = 0;
	env->btree_mem_used = 0;
	env->mem_used = 0;
	env->mem_quota = 16 * 1024 * 1024;
	memset(&env->stat, 0, sizeof(env->stat));
	memset(&env->io, 0, sizeof(env->io));
	mempool_create(&env->cache_node_mempool, slab_cache,
		       sizeof(struct vy_page_index_cache_node));
	matras_allocator_create(&env->allocator,
			        VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE,
			        vy_page_index_cache_tree_page_alloc,
			        vy_page_index_cache_tree_page_free);
}

void
vy_page_index_cache_env_destroy(struct vy_page_index_cache_env *env)
{
	matras_allocator_destroy(&env->allocator);
	mempool_destroy(&env->cache_node_mempool);
}

static inline size_t
vy_page_index_cache_node_size(const struct vy_page_index_cache_node *node)
{
	size_t size = sizeof(*node);
	if (node->entry.min_key != NULL)
		size += mp_len(node->entry.min_key);
	return size;
}

static struct vy_page_index_cache_node *
vy_page_index_cache_node_new(struct vy_page_index_cache_env *env,
			     struct vy_page_index_cache *cache,
			     struct vy_page_index_entry *entry)
{
	struct vy_page_index_cache_node *node =
		mempool_alloc(&env->cache_node_mempool);
	if (node == NULL)
		return NULL;
	memset(node, 0, sizeof(*node));
	node->cache = cache;
	node->entry = vy_page_index_entry_copy(entry);
	rlist_add_entry(&env->cache_lru, node, in_lru);
	env->mem_used += vy_page_index_cache_node_size(node);
	return node;
}

static void
vy_page_index_cache_node_delete(struct vy_page_index_cache_env *env,
				struct vy_page_index_cache_node *node)
{
	assert(env->mem_used >= vy_page_index_cache_node_size(node));
	env->mem_used -= vy_page_index_cache_node_size(node);
	rlist_del_entry(node, in_lru);
	vy_page_index_entry_destroy(&node->entry);
	mempool_free(&env->cache_node_mempool, node);
}

/* Touch a node to move it to the front of the LRU list. */
static inline void
vy_page_index_cache_touch(struct vy_page_index_cache_node *node)
{
	rlist_move_entry(&node->cache->env->cache_lru, node, in_lru);
}

static void
vy_page_index_cache_create(struct vy_page_index_cache *cache,
			   struct vy_page_index_cache_env *env,
			   struct key_def *cmp_def,
			   uint32_t page_count)
{
	memset(cache, 0, sizeof(*cache));
	cache->cmp_def = key_def_dup(cmp_def);
	cache->env = env;
	matras_stats_create(&cache->matras_stats);
	vy_page_index_cache_tree_create(&cache->cache_tree, cache->cmp_def,
					&env->allocator, &cache->matras_stats);
	cache->page_count = page_count;
}

static void
vy_page_index_cache_destroy(struct vy_page_index_cache *cache)
{
	if (cache->env == NULL)
		return;
	struct vy_page_index_cache_tree_iterator it =
		vy_page_index_cache_tree_first(&cache->cache_tree);
	while (!vy_page_index_cache_tree_iterator_is_invalid(&it)) {
		struct vy_page_index_cache_node **node =
			vy_page_index_cache_tree_iterator_get_elem(
				&cache->cache_tree, &it);
		assert(node != NULL && *node != NULL);
		vy_page_index_cache_tree_iterator_next(&cache->cache_tree, &it);
		vy_page_index_cache_node_delete(cache->env, *node);
	}
	vy_page_index_cache_tree_destroy(&cache->cache_tree);
	TRASH(cache);
}

static void
vy_page_index_cache_gc_step(struct vy_page_index_cache_env *env)
{
	struct rlist *lru = &env->cache_lru;
	/* Evict the last recently used. */
	struct vy_page_index_cache_node *node =
		rlist_last_entry(lru, struct vy_page_index_cache_node, in_lru);
	struct vy_page_index_cache *cache = node->cache;
	//vy_stmt_counter_acct_tuple(&cache->stat.evict, node->info);
	vy_page_index_cache_tree_delete(&cache->cache_tree, node, NULL);
	vy_page_index_cache_node_delete(cache->env, node);
	/* Update eviction stats. */
	env->stat.evict++;
}

static void
vy_page_index_cache_gc(struct vy_page_index_cache_env *env)
{
	for (uint32_t i = 0; i < VY_PAGE_INDEX_CACHE_CLEANUP_MAX_STEPS; i++) {
		if (env->mem_used <= env->mem_quota)
			break;
		vy_page_index_cache_gc_step(env);
	}
}

static void MAYBE_UNUSED
vy_page_index_cache_env_set_quota(struct vy_page_index_cache_env *env,
				  size_t quota)
{
	env->mem_quota = quota;
	while (env->mem_used > env->mem_quota) {
		vy_page_index_cache_gc(env);
		/* Make sure we don't block other tx fibers for too long. */
		fiber_sleep(0);
	}
}

/**
 * Add chain to cache.
 * @param cache - cache
 * @param next - next statement
 * @param prev - previous statement
 */
static void
vy_page_index_cache_add_chain(struct vy_page_index_cache *cache,
			      struct vy_page_index_entry *next,
			      struct vy_page_index_entry *prev)
{
	assert(cache->page_count > 0);
	assert(prev->idx + 1 == next->idx);

	if (cache->env->mem_quota == 0) {
		/* Cache is disabled. */
		return;
	}

	/* Evict some entries if quota overused. */
	vy_page_index_cache_gc(cache->env);

	struct vy_page_index_cache_node *replaced = NULL;

	/* Insert only prev node. */
	if (next->idx == (int64_t)cache->page_count) {
		struct vy_page_index_cache_node *prev_node =
			vy_page_index_cache_node_new(cache->env, cache, prev);
		if (prev_node == NULL)
			return;
		struct vy_page_index_cache_node *successor = NULL;
		if (vy_page_index_cache_tree_insert(
			&cache->cache_tree, prev_node, &replaced, &successor) != 0) {
			/* memory error, let's live without a cache */
			vy_page_index_cache_node_delete(cache->env, prev_node);
			return;
		}
		assert(successor == NULL);
		/* May race with another fiber filling the same cache gap during I/O. */
		if (replaced != NULL)
			vy_page_index_cache_node_delete(cache->env, replaced);
		return;
	}

	/* Insert next node. */
	struct vy_page_index_cache_node *next_node =
		vy_page_index_cache_node_new(cache->env, cache, next);
	if (next_node == NULL)
		return;

	struct vy_page_index_cache_tree_iterator inserted;
	if (vy_page_index_cache_tree_insert_get_iterator(
	    &cache->cache_tree, next_node, &replaced, &inserted) != 0) {
		/* memory error, let's live without a cache */
		vy_page_index_cache_node_delete(cache->env, next_node);
		return;
	}
	assert(!vy_page_index_cache_tree_iterator_is_invalid(&inserted));
	/* May race with another fiber filling the same cache gap during I/O. */
	if (replaced != NULL)
		vy_page_index_cache_node_delete(cache->env, replaced);

	/* Check that prev needs to be inserted. */
	vy_page_index_cache_tree_iterator_prev(&cache->cache_tree, &inserted);

	if (prev->idx < 0) {
		assert(vy_page_index_cache_tree_iterator_is_invalid(&inserted));
		return;
	}

	if (!vy_page_index_cache_tree_iterator_is_invalid(&inserted)) {
		struct vy_page_index_cache_node **prev_check_node =
			vy_page_index_cache_tree_iterator_get_elem(
				&cache->cache_tree, &inserted);
		assert(*prev_check_node != NULL);
		struct vy_page_index_entry *prev_check =
			&(*prev_check_node)->entry;
#ifndef NDEBUG
		int cmp = vy_page_index_entry_compare(prev_check, prev,
						      cache->cmp_def);
#endif
		assert(cmp <= 0);
		if (prev_check->idx + 1 == next->idx) {
			/* The found node must be exactly prev. */
			assert(cmp == 0);
			return;
		}
	}

	/* There is no such node - insert it. */
	struct vy_page_index_cache_node *prev_node =
		vy_page_index_cache_node_new(cache->env, cache, prev);
	if (prev_node == NULL)
		return;
	replaced = NULL;
	struct vy_page_index_cache_node *successor = NULL;
	if (vy_page_index_cache_tree_insert(
		&cache->cache_tree, prev_node, &replaced, &successor) != 0) {
		/* memory error, let's live without a cache */
		vy_page_index_cache_node_delete(cache->env, prev_node);
		return;
	}
	/* May race with another fiber filling the same cache gap during I/O. */
	if (replaced != NULL)
		vy_page_index_cache_node_delete(cache->env, replaced);
	else
		assert(successor == next_node);
}

/* Find the chain of elements for the given key. */
static bool
vy_page_index_cache_find_chain(struct vy_page_index_cache *cache,
			       struct vy_entry key, bool lower_bound,
			       struct vy_page_index_entry *next,
			       struct vy_page_index_entry *prev,
			       bool *equal_key)
{
	struct vy_page_index_cache_tree_iterator it;
	if (lower_bound)
		it = vy_page_index_cache_tree_lower_bound(&cache->cache_tree,
							  key, equal_key);
	else
		it = vy_page_index_cache_tree_upper_bound(&cache->cache_tree,
							  key, equal_key);

	struct vy_page_index_cache_node **next_node =
		vy_page_index_cache_tree_iterator_get_elem(
			&cache->cache_tree, &it);

	if (next_node == NULL) {
		next->idx = cache->page_count;
		next->min_key = NULL;
		next->min_key_hint = HINT_NONE;
		it = vy_page_index_cache_tree_last(&cache->cache_tree);
	} else {
		/* Move to the front of the LRU list. */
		vy_page_index_cache_touch(*next_node);
		*next = vy_page_index_entry_copy(&(*next_node)->entry);
		if (next->idx == 0) {
			prev->idx = -1;
			prev->min_key = NULL;
			prev->min_key_hint = HINT_NONE;
			return true;
		}
		vy_page_index_cache_tree_iterator_prev(&cache->cache_tree, &it);
	}

	struct vy_page_index_cache_node **prev_node =
		vy_page_index_cache_tree_iterator_get_elem(
			&cache->cache_tree, &it);
	if (prev_node == NULL)
		return false;
	vy_page_index_cache_touch(*prev_node);
	*prev = vy_page_index_entry_copy(&(*prev_node)->entry);

	/* It is a chain. */
	if (prev->idx + 1 == next->idx)
		return true;

	vy_page_index_entry_destroy(next);
	vy_page_index_entry_destroy(prev);
	return false;
}

/* }}} Page Index Cache */


/**
 * {{{ Page Info Cache - a cache of page info blocks.
 * Used to get page info by page number.
 */

static void *
vy_page_info_cache_tree_page_alloc(struct matras_allocator *allocator)
{
	struct vy_page_info_cache_env *env =
		container_of(allocator, struct vy_page_info_cache_env, allocator);
	env->tree_mem_used += VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE;
	return xmalloc(VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE);
}

static void
vy_page_info_cache_tree_page_free(struct matras_allocator *allocator, void *ptr)
{
	struct vy_page_info_cache_env *env =
		container_of(allocator, struct vy_page_info_cache_env, allocator);
	assert(env->tree_mem_used >= VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE);
	env->tree_mem_used -= VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE;
	free(ptr);
}

void
vy_page_info_cache_env_create(struct vy_page_info_cache_env *env,
			       struct slab_cache *slab_cache)
{
	rlist_create(&env->cache_lru);
	env->tree_mem_used = 0;
	env->mem_used = 0;
	env->mem_quota = 64 * 1024 * 1024;
	memset(&env->stat, 0, sizeof(env->stat));
	memset(&env->io, 0, sizeof(env->io));
	mempool_create(&env->cache_node_mempool, slab_cache,
		       sizeof(struct vy_page_info_cache_node));
	matras_allocator_create(&env->allocator,
			        VY_PAGE_INFO_CACHE_TREE_EXTENT_SIZE,
			        vy_page_info_cache_tree_page_alloc,
			        vy_page_info_cache_tree_page_free);
}

void
vy_page_info_cache_env_destroy(struct vy_page_info_cache_env *env)
{
	matras_allocator_destroy(&env->allocator);
	mempool_destroy(&env->cache_node_mempool);
}

static inline size_t
vy_page_info_cache_node_size(const struct vy_page_info_cache_node *node)
{
	size_t size = sizeof(*node);
	const struct vy_page_info_block *block = &node->block;

	for (uint32_t i = 0; i < block->r - block->l; i++) {
		struct vy_page_info *page = block->data[i];
		if (page != NULL)
			size += vy_page_info_memory(page);
	}
	return size;
}

static struct vy_page_info_cache_node *
vy_page_info_cache_node_new(struct vy_page_info_cache_env *env,
			     struct vy_page_info_cache *cache,
			     const struct vy_page_info_block *block)
{
	struct vy_page_info_cache_node *node =
		mempool_alloc(&env->cache_node_mempool);
	if (node == NULL)
		return NULL;
	memset(node, 0, sizeof(*node));
	node->cache = cache;
	node->block = *block;
	rlist_add_entry(&env->cache_lru, node, in_lru);
	env->mem_used += vy_page_info_cache_node_size(node);
	return node;
}

static void
vy_page_info_cache_node_delete(struct vy_page_info_cache_env *env,
				struct vy_page_info_cache_node *node)
{
	assert(env->mem_used >= vy_page_info_cache_node_size(node));
	env->mem_used -= vy_page_info_cache_node_size(node);
	rlist_del_entry(node, in_lru);
	vy_page_info_block_destroy(&node->block);
	mempool_free(&env->cache_node_mempool, node);
}

static void
vy_page_info_cache_create(struct vy_page_info_cache *cache,
			  struct vy_page_info_cache_env *env,
			  uint32_t page_count)
{
	assert(env != NULL);
	memset(cache, 0, sizeof(*cache));
	cache->env = env;
	matras_stats_create(&cache->matras_stats);
	vy_page_info_cache_tree_create(&cache->cache_tree, NULL,
				       &env->allocator, &cache->matras_stats);
	cache->page_count = page_count;
}

static void
vy_page_info_cache_destroy(struct vy_page_info_cache *cache)
{
	if (cache->env == NULL)
		return;
	struct vy_page_info_cache_tree_iterator it =
		vy_page_info_cache_tree_first(&cache->cache_tree);
	while (!vy_page_info_cache_tree_iterator_is_invalid(&it)) {
		struct vy_page_info_cache_node **node =
			vy_page_info_cache_tree_iterator_get_elem(
				&cache->cache_tree, &it);
		assert(node != NULL && *node != NULL);
		vy_page_info_cache_tree_iterator_next(&cache->cache_tree, &it);
		vy_page_info_cache_node_delete(cache->env, *node);
	}
	vy_page_info_cache_tree_destroy(&cache->cache_tree);
	TRASH(cache);
}

static void
vy_page_info_cache_gc_step(struct vy_page_info_cache_env *env)
{
	struct rlist *lru = &env->cache_lru;
	struct vy_page_info_cache_node *node, *tmp;
	rlist_foreach_entry_safe_reverse(node, lru, in_lru, tmp) {
		if (node->pin_count != 0)
			continue;
		struct vy_page_info_cache *cache = node->cache;
		vy_page_info_cache_tree_delete(&cache->cache_tree, node, NULL);
		vy_page_info_cache_node_delete(cache->env, node);
		/* Update eviction stats. */
		env->stat.evict++;
		return;
	}
	/* All nodes are pinned, nothing to evict. */
}

static void
vy_page_info_cache_gc(struct vy_page_info_cache_env *env)
{
	for (uint32_t i = 0; i < VY_PAGE_INFO_CACHE_CLEANUP_MAX_STEPS; i++) {
		if (env->mem_used <= env->mem_quota)
			break;
		vy_page_info_cache_gc_step(env);
	}
}

static void MAYBE_UNUSED
vy_page_info_cache_env_set_quota(struct vy_page_info_cache_env *env,
				  size_t quota)
{
	env->mem_quota = quota;
	while (env->mem_used > env->mem_quota) {
		vy_page_info_cache_gc(env);
		/* Make sure we don't block other tx fibers for too long. */
		fiber_sleep(0);
	}
}

/* Pin a node to prevent it from being evicted. */
static inline void
vy_page_info_cache_node_pin(struct vy_page_info_cache_node *node)
{
	node->pin_count++;
	/* Update pinned stats. */
	node->cache->env->stat.pinned++;
}

/* Unpin a node to allow it to be evicted. */
static inline void
vy_page_info_cache_node_unpin(struct vy_page_info_cache_node *node)
{
	assert(node->pin_count > 0);
	node->pin_count--;
	/* Update pinned stats. */
	node->cache->env->stat.pinned--;
}

/* Touch a node to move it to the front of the LRU list. */
static inline void
vy_page_info_cache_touch(struct vy_page_info_cache_node *node)
{
	rlist_move_entry(&node->cache->env->cache_lru, node, in_lru);
}

/* Add a block to the cache. */
static int
vy_page_info_cache_add_block(struct vy_page_index_array *array,
			     struct vy_page_info_block *block,
			     struct vy_page_info_cache_tree_iterator *it,
			     struct vy_page_info_cache_node **node)
{
	struct vy_page_info_cache *cache = &array->cache;
	vy_page_info_cache_gc(cache->env);

	/*
	 * Check if the block was already added to the cache (e.g. by another
	 * fiber while we were doing I/O). Use the tree element comparator here:
	 * key upper_bound searches by block.r and may find an adjacent block.
	 */
	struct vy_page_info_cache_node key_node;
	memset(&key_node, 0, sizeof(key_node));
	key_node.block.l = block->l;
	bool exact = false;
	*it = vy_page_info_cache_tree_lower_bound_elem(
		&cache->cache_tree, &key_node, &exact);
	struct vy_page_info_cache_node **existing =
		vy_page_info_cache_tree_iterator_get_elem(
			&cache->cache_tree, it);
	if (exact) {
		assert(existing != NULL && *existing != NULL);
		*node = *existing;
		vy_page_info_block_destroy(block);
		return 0;
	}

	struct vy_page_info_cache_node *new_node =
		vy_page_info_cache_node_new(cache->env, cache, block);
	if (new_node == NULL) {
		diag_set(OutOfMemory, sizeof(*new_node), "mempool_alloc",
			 "struct vy_page_info_cache_node");
		return -1;
	}

	struct vy_page_info_cache_node *replaced = NULL;
	if (vy_page_info_cache_tree_insert_get_iterator(
		&cache->cache_tree, new_node, &replaced, it) != 0) {
		vy_page_info_cache_node_delete(cache->env, new_node);
		return -1;
	}
	if (replaced != NULL)
		vy_page_info_cache_node_delete(cache->env, replaced);
	struct vy_page_info_cache_node **node_ptr =
		vy_page_info_cache_tree_iterator_get_elem(
			&cache->cache_tree, it);
	assert(node_ptr != NULL && *node_ptr != NULL);
	*node = *node_ptr;
	return 0;
}

/* }}} Page Info Cache */

/**
 * {{{ Page Index Array - a sorted array of page info blocks. 
 * Used to get page info by page number.
 *
 * Contains fixed size entries in .offsets file to quickly get
 * the offset in the .index file by page number.
 * Also uses Page Info Cache for page info blocks.
 *
 * Getting page_info in the worst case requires 2 I/O operations:
 * 1. Read the offset from the .offsets file.
 * 2. Read the page info block from the .index file.
 */

void
vy_page_info_block_destroy(struct vy_page_info_block *block)
{
	assert(block->r >= block->l);
	for (uint32_t i = 0; i < block->r - block->l; i++)
		vy_page_info_delete(block->data[i]);
	TRASH(block);
}

static int
vy_page_info_read_index(const char *index_filepath,
			uint64_t *offsets, uint32_t page_count)
{
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, index_filepath) != 0) {
		free(offsets);
		return -1;
	}
	if (strcmp(cursor.meta.filetype, XLOG_META_TYPE_INDEX) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_INDEX, cursor.meta.filetype);
		goto fail;
	}
	/* Skip run info transaction. */
	if (xlog_cursor_next_tx(&cursor) != 0) {
		vy_page_index_invalid_diag_set(
			index_filepath, "Missing run info transaction");
		goto fail;
	}
	struct xrow_header xrow;
	if (xlog_cursor_next_row(&cursor, &xrow) != 0 ||
	    xrow.type != VY_INDEX_RUN_INFO) {
		vy_page_index_invalid_diag_set(
			index_filepath, "Missing run info row");
		goto fail;
	}
	for (uint32_t i = 0; i < page_count; i++) {
		offsets[i] = xlog_cursor_pos(&cursor);
		if (xlog_cursor_next_tx(&cursor) != 0) {
			vy_page_index_invalid_diag_set(
				index_filepath,
				"Missing page info transaction");
			goto fail;
		}
		int rc = xlog_cursor_next_row(&cursor, &xrow);
		if (rc != 0) {
			vy_page_index_invalid_diag_set(
				index_filepath,
				"Failed to read page info row");
			goto fail;
		}
		if (xrow.type != VY_INDEX_PAGE_INFO) {
			vy_page_index_invalid_diag_set(
				index_filepath, "Missing page info row");
			goto fail;
		}
	}
	xlog_cursor_close(&cursor, false);
	return 0;
fail:
	xlog_cursor_close(&cursor, false);
	return -1;
}

static void
vy_page_index_array_create(struct vy_page_index_array *array,
			  struct vy_page_info_cache_env *env,
			  struct key_def *cmp_def,
			  const char *index_filepath,
			  const char *index_offsets_filepath,
			  uint32_t page_count)
{
	array->page_count = page_count;
	array->cmp_def = key_def_dup(cmp_def);
	vy_page_info_cache_create(&array->cache, env, page_count);
	array->index_filepath = strdup(index_filepath);
	array->index_offsets_filepath = strdup(index_offsets_filepath);
	array->index_fd = -1;
	array->index_offsets_fd = -1;
}

static void
vy_page_index_array_destroy(struct vy_page_index_array *array)
{
	if (array->index_fd >= 0)
		close(array->index_fd);
	if (array->index_offsets_fd >= 0)
		close(array->index_offsets_fd);
	free((char *)array->index_filepath);
	free((char *)array->index_offsets_filepath);
	key_def_delete(array->cmp_def);
	vy_page_info_cache_destroy(&array->cache);
}

static int
vy_page_index_array_open(struct vy_page_index_array *array)
{
	if (array->index_fd < 0) {
		int fd = open(array->index_filepath, O_RDONLY);
		if (fd < 0) {
			diag_set(SystemError, "failed to open %s",
				 array->index_filepath);
			return -1;
		}
		array->index_fd = fd;
	}
	if (array->index_offsets_fd < 0) {
		int fd = open(array->index_offsets_filepath, O_RDONLY);
		if (fd < 0) {
			diag_set(SystemError, "failed to open %s",
				 array->index_offsets_filepath);
			close(array->index_fd);
			return -1;
		}
		array->index_offsets_fd = fd;
	}
	return 0;
}

static int
vy_page_index_array_build(uint32_t page_count,
			  const char *index_path,
			  const char *index_offsets_path,
			  struct vy_page_info_cache_env *env)
{
	uint64_t *offsets = calloc(page_count, sizeof(uint64_t));
	if (offsets == NULL) {
		diag_set(OutOfMemory, page_count * sizeof(uint64_t),
			 "calloc", "page info offsets");
		return -1;
	}
	/* Read offsets from the .index file. */
	if (vy_page_info_read_index(index_path, offsets, page_count) != 0) {
		free(offsets);
		return -1;
	}
	int fd = open(index_offsets_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		diag_set(SystemError, "failed to create %s", index_offsets_path);
		free(offsets);
		return -1;
	}
	if (fio_writen(fd, &page_count, sizeof(page_count)) != 0 ||
	    fio_writen(fd, offsets, page_count * sizeof(uint64_t)) != 0) {
		diag_set(SystemError, "failed to write to %s", index_offsets_path);
		close(fd);
		free(offsets);
		return -1;
	}
	close(fd);
	free(offsets);
	/* Update IO stats. */
	if (env != NULL) {
		env->io.write_bytes += (int64_t)(sizeof(page_count) +
			(size_t)page_count * sizeof(uint64_t));
	}
	return 0;
}

static inline void
vy_page_index_offsets_invalid_diag_set(
	const char *filename, const char *message)
{
	diag_set(ClientError, ER_INVALID_INDEX_OFFSETS_FILE,
		 filename, message);
}

/**
 * Read offsets for pages [l, r) from the index_offsets file.
 * Also reads offset[r] for the end boundary if r < page_count.
 * The returned array uses zero-based indexing relative to l.
 */
static int
vy_page_index_array_read_block_offsets(struct vy_page_index_array *array,
				      uint32_t l, uint32_t r,
				      uint64_t **offsets_out)
{
	uint32_t count = r - l;
	if (r < array->page_count)
		count++;
	uint64_t *offsets = malloc(count * sizeof(uint64_t));
	if (offsets == NULL) {
		diag_set(OutOfMemory, count * sizeof(uint64_t),
			 "malloc", "page info offsets");
		return -1;
	}
	assert(array->index_offsets_fd >= 0);
	off_t file_offset = sizeof(uint32_t) +
			    (off_t)l * sizeof(uint64_t);
	size_t size = count * sizeof(uint64_t);
	ssize_t nrd = fio_pread(array->index_offsets_fd, offsets,
				size, file_offset);
	if (nrd < 0 || (size_t)nrd != size) {
		vy_page_index_offsets_invalid_diag_set(
			array->index_offsets_filepath,
			"Failed to read offsets");
		free(offsets);
		return -1;
	}
	if (array->cache.env != NULL) {
		pm_atomic_fetch_add(&array->cache.env->io.read_bytes, nrd);
		pm_atomic_fetch_add(&array->cache.env->io.read_ops, 1);
	}
	*offsets_out = offsets;
	return 0;
}

/* Read a block of page info from the .index file. */
static int
vy_page_index_array_read_block(struct vy_page_index_array *array,
			      uint32_t block_idx,
			      struct vy_page_info_block *result)
{
	assert(array->cmp_def != NULL);
	uint32_t l = block_idx * VY_PAGE_INFO_BLOCK;
	assert(l < array->page_count);
	uint32_t r = MIN(array->page_count, l + VY_PAGE_INFO_BLOCK);

	uint64_t *offsets;
	if (vy_page_index_array_read_block_offsets(array, l, r, &offsets) != 0)
		return -1;

	memset(result, 0, sizeof(*result));
	result->l = l;
	result->r = r;
	assert(array->index_fd >= 0);
	struct stat st;
	if (fstat(array->index_fd, &st) != 0) {
		diag_set(SystemError, "fstat failed");
		free(offsets);
		return -1;
	}
	/*
	 * Read the whole block range at once. The offsets point to xlog
	 * transactions; each page_info payload starts after the transaction
	 * fixheader and ends at the next transaction offset.
	 */
	off_t block_start = (off_t)offsets[0] + XLOG_FIXHEADER_SIZE;
	off_t block_end = r < array->page_count ?
			  (off_t)offsets[r - l] : st.st_size;
	if (block_start >= block_end) {
		vy_page_index_offsets_invalid_diag_set(
			array->index_offsets_filepath,
			"Invalid page_info block offset");
		goto fail_offsets;
	}
	size_t block_size = (size_t)(block_end - block_start);
	char *data = malloc(block_size);
	if (data == NULL) {
		diag_set(OutOfMemory, block_size, "malloc", "page info block");
		goto fail_offsets;
	}
	ssize_t readen = fio_pread(array->index_fd, data, block_size,
				  block_start);
	if (readen < 0) {
		diag_set(SystemError, "failed to read from file");
		goto fail;
	}
	if (array->cache.env != NULL) {
		pm_atomic_fetch_add(&array->cache.env->io.read_bytes, readen);
		pm_atomic_fetch_add(&array->cache.env->io.read_ops, 1);
	}
	if (readen != (ssize_t)block_size) {
		vy_page_index_offsets_invalid_diag_set(
			array->index_offsets_filepath,
			"Unexpected end of file");
		goto fail;
	}
	for (uint32_t page_no = l; page_no < r; page_no++) {
		off_t tx_offset = (off_t)offsets[page_no - l];
		off_t next_tx_offset = page_no + 1 < array->page_count ?
				       (off_t)offsets[page_no - l + 1] :
				       st.st_size;
		off_t offset = tx_offset + XLOG_FIXHEADER_SIZE;
		off_t next_offset = next_tx_offset;
		if (offset < block_start || offset >= next_offset ||
		    next_offset > block_end) {
			vy_page_index_offsets_invalid_diag_set(
				array->index_offsets_filepath,
				"Invalid page_info offset");
			goto fail;
		}
		const char *pos = data + (offset - block_start);
		const char *end = data + (next_offset - block_start);
		struct xrow_header xrow;
		if (xrow_decode(&xrow, &pos, end, false) != 0 ||
		    xrow.type != VY_INDEX_PAGE_INFO) {
			vy_page_index_offsets_invalid_diag_set(
				array->index_offsets_filepath,
				"Can't read page_info row");
			goto fail;
		}
		struct vy_page_info *page = vy_page_info_new();
		if (page == NULL)
			goto fail;
		if (vy_page_info_decode(page, &xrow, array->cmp_def,
					array->index_filepath) != 0) {
			vy_page_info_delete(page);
			goto fail;
		}
		result->data[page_no - l] = page;
	}
	free(data);
	free(offsets);
	return 0;
fail:
	free(data);
fail_offsets:
	free(offsets);
	vy_page_info_block_destroy(result);
	return -1;
}

int
vy_page_index_read_page_info_block(struct vy_page_index *index,
				   uint32_t block_idx,
				   struct vy_page_info_block *result)
{
	if (vy_page_index_array_open(&index->page_info) != 0)
		return -1;
	return vy_page_index_array_read_block(&index->page_info, block_idx,
					      result);
}

struct vy_page_index_array_iterator
vy_page_index_array_invalid_iterator(void)
{
	return (struct vy_page_index_array_iterator){
		.cache_it = vy_page_info_cache_tree_invalid_iterator(),
		.node = NULL,
		.page_no = UINT32_MAX,
	};
}

static inline bool
vy_page_index_array_iterator_is_invalid(struct vy_page_index_array_iterator *it)
{
	return it->page_no == UINT32_MAX;
}

static inline void
vy_page_index_array_iterator_set_node(struct vy_page_index_array *array,
				     struct vy_page_index_array_iterator *it,
				     struct vy_page_info_cache_tree_iterator *cache_it,
				     struct vy_page_info_cache_node *node)
{
	(void)array;
	assert(node != NULL);
	/* Unpin the previous node to allow it to be evicted. */
	if (it->node != NULL)
		vy_page_info_cache_node_unpin(it->node);
	/* Pin the node to prevent it from being evicted. */
	vy_page_info_cache_node_pin(node);
	/* Touch the node to move it to the front of the LRU list. */
	vy_page_info_cache_touch(node);
	it->cache_it = *cache_it;
	it->node = node;
}

void
vy_page_index_array_iterator_close(struct vy_page_index_array_iterator *it)
{
	if (it->node != NULL)
		vy_page_info_cache_node_unpin(it->node);
	*it = vy_page_index_array_invalid_iterator();
}

struct vy_page_info *
vy_page_index_array_iterator_get(struct vy_page_index_array_iterator *it)
{
	if (vy_page_index_array_iterator_is_invalid(it))
		return NULL;
	assert(it->node != NULL);
	assert(it->node->block.l <= it->page_no &&
	       it->page_no < it->node->block.r);
	return it->node->block.data[it->page_no - it->node->block.l];
}

/* Read a block of page info from the .index file to the cache. */
static int
vy_page_index_array_read_block_to_cache(struct vy_page_index_array *array,
				       uint32_t block_idx,
				       struct vy_page_info_cache_tree_iterator *cache_it)
{
	/* Update miss stats. */
	array->cache.env->stat.miss++;
	if (vy_page_index_array_open(array) != 0)
		return -1;
	struct vy_page_info_block_read_task task;
	memset(&task, 0, sizeof(task));
	task.array = array;
	task.block_idx = block_idx;
	/* The COIO callback reads from disk; cache insertion stays here. */
	if (vy_run_env_coio_call(array->cache.env->run_env, &task.base,
				 vy_page_info_block_read_cb) != 0)
		return -1;
	struct vy_page_info_cache_node *node = NULL;
	return vy_page_info_cache_add_block(array, &task.block, cache_it, &node);
}

int
vy_page_index_array_iterator_next(struct vy_page_index_array *array,
				 struct vy_page_index_array_iterator *it)
{
	if (vy_page_index_array_iterator_is_invalid(it))
		return 0;
	assert(!vy_page_info_cache_tree_iterator_is_invalid(&it->cache_it));
	assert(it->node != NULL);

	++it->page_no;
	if (it->page_no >= array->page_count) {
		vy_page_index_array_iterator_close(it);
		return 0;
	}

	struct vy_page_info_block *block = &it->node->block;
	if (it->page_no < block->r)
		return 0;

	assert(it->page_no % VY_PAGE_INFO_BLOCK == 0);

	struct vy_page_info_cache_tree_iterator cache_it = it->cache_it;
	vy_page_info_cache_tree_iterator_next(
		&array->cache.cache_tree, &cache_it);
	struct vy_page_info_cache_node **next_node =
		vy_page_info_cache_tree_iterator_get_elem(
			&array->cache.cache_tree, &cache_it);
	if (next_node != NULL && *next_node != NULL) {
		struct vy_page_info_block *next_block = &(*next_node)->block;
		if (it->page_no == next_block->l) {
			/* Update hit stats. */
			array->cache.env->stat.hit++;
			vy_page_index_array_iterator_set_node(
				array, it, &cache_it, *next_node);
			return 0;
		}
	}
	/* Go to disk. */
	uint32_t block_idx = it->page_no / VY_PAGE_INFO_BLOCK;
	if (vy_page_index_array_read_block_to_cache(array, block_idx, &cache_it) != 0)
		return -1;
	next_node = vy_page_info_cache_tree_iterator_get_elem(
		&array->cache.cache_tree, &cache_it);
	assert(next_node != NULL && *next_node != NULL);
	vy_page_index_array_iterator_set_node(array, it, &cache_it, *next_node);
	return 0;
}

int
vy_page_index_array_iterator_prev(struct vy_page_index_array *array,
				 struct vy_page_index_array_iterator *it)
{
	if (vy_page_index_array_iterator_is_invalid(it))
		return 0;
	assert(!vy_page_info_cache_tree_iterator_is_invalid(&it->cache_it));
	assert(it->node != NULL);

	if (it->page_no == 0) {
		vy_page_index_array_iterator_close(it);
		return 0;
	}
	--it->page_no;

	struct vy_page_info_block *block = &it->node->block;
	if (block->l <= it->page_no)
		return 0;

	assert((it->page_no + 1) % VY_PAGE_INFO_BLOCK == 0);

	struct vy_page_info_cache_tree_iterator cache_it = it->cache_it;
	vy_page_info_cache_tree_iterator_prev(
		&array->cache.cache_tree, &cache_it);
	struct vy_page_info_cache_node **prev_node =
		vy_page_info_cache_tree_iterator_get_elem(
			&array->cache.cache_tree, &cache_it);
	if (prev_node != NULL && *prev_node != NULL) {
		struct vy_page_info_block *prev_block = &(*prev_node)->block;
		if (it->page_no + 1 == prev_block->r) {
			/* Update hit stats. */
			array->cache.env->stat.hit++;
			vy_page_index_array_iterator_set_node(
				array, it, &cache_it, *prev_node);
			return 0;
		}
	}
	/* Go to disk. */
	uint32_t block_idx = it->page_no / VY_PAGE_INFO_BLOCK;
	if (vy_page_index_array_read_block_to_cache(array, block_idx, &cache_it) != 0)
		return -1;
	prev_node = vy_page_info_cache_tree_iterator_get_elem(
		&array->cache.cache_tree, &cache_it);
	assert(prev_node != NULL && *prev_node != NULL);
	vy_page_index_array_iterator_set_node(array, it, &cache_it, *prev_node);
	return 0;
}

static int
vy_page_index_array_get_page(struct vy_page_index_array *array,
			     uint32_t page_no,
			     struct vy_page_index_array_iterator *it)
{
	*it = vy_page_index_array_invalid_iterator();
	if (page_no >= array->page_count)
		return 0;

	bool unused;
	struct vy_page_info_cache_tree_iterator cache_it =
		vy_page_info_cache_tree_upper_bound(
			&array->cache.cache_tree, page_no, &unused);
	struct vy_page_info_cache_node **node =
		vy_page_info_cache_tree_iterator_get_elem(
			&array->cache.cache_tree, &cache_it);
	if (node != NULL && *node != NULL && (*node)->block.l <= page_no) {
		/* Update hit stats. */
		array->cache.env->stat.hit++;
		assert(page_no < (*node)->block.r);
		it->page_no = page_no;
		vy_page_index_array_iterator_set_node(
			array, it, &cache_it, *node);
		return 0;
	}
	/* Go to disk. */
	it->page_no = page_no;
	uint32_t block_idx = page_no / VY_PAGE_INFO_BLOCK;
	if (vy_page_index_array_read_block_to_cache(array, block_idx, &cache_it) != 0)
		return -1;
	node = vy_page_info_cache_tree_iterator_get_elem(
		&array->cache.cache_tree, &cache_it);
	assert(node != NULL && *node != NULL);
	vy_page_index_array_iterator_set_node(
		array, it, &cache_it, *node);
	return 0;
}

/*
 * .page_info layout (to be implemented):
 *   tx1: META
 *   tx2: OFFSETS xrow (mp_bin of uint64 offsets)
 *   tx3..: VY_INDEX_PAGE_INFO rows (same payload as in .index)
 */
/* }}} Page Index Array */

/* {{{ Page Index */

static void
vy_page_index_create(struct vy_page_index *index,
		     const char *index_path,
		     const char *index_btree_path,
		     const char *index_offsets_path,
		     struct vy_page_index_cache_env *page_index_cache_env,
		     struct vy_page_info_cache_env *page_info_cache_env,
		     struct key_def *cmp_def,
		     uint32_t page_count,
		     uint64_t btree_root_offset,
		     uint64_t btree_data_offset)
{
	index->page_count = page_count;
	vy_page_index_cache_create(&index->cache, page_index_cache_env,
				   cmp_def, page_count);
	vy_page_index_btree_create(&index->btree, cmp_def,
				   page_index_cache_env,
				   btree_root_offset, btree_data_offset,
				   index_btree_path,
				   page_count);
	vy_page_index_array_create(&index->page_info,
				  page_info_cache_env,
				  cmp_def, index_path, index_offsets_path,
				  page_count);
}

void
vy_page_index_destroy(struct vy_page_index *index)
{
	vy_page_index_cache_destroy(&index->cache);
	vy_page_index_btree_destroy(&index->btree);
	vy_page_index_array_destroy(&index->page_info);
	TRASH(index);
}

/* Recover the page index from the .index, .btree and .offsets files. */
int
vy_page_index_recover(struct vy_page_index *index,
		      const char *index_path,
		      const char *index_btree_path,
		      const char *index_offsets_path,
		      struct vy_page_index_cache_env *page_index_cache_env,
		      struct vy_page_info_cache_env *page_info_cache_env,
		      struct key_def *cmp_def,
		      struct vy_page_info *page_info_array, uint32_t page_count)
{
	/* Write .btree file if not exists. */
	uint64_t btree_root_offset = 0;
	uint64_t btree_data_offset = 0;
	if (vy_page_index_btree_read_meta(index_btree_path, &btree_root_offset,
					  &btree_data_offset) != 0) {
		if (errno != ENOENT || page_count == 0)
			return -1;
		diag_clear(diag_get());
		say_warn("missing page index file `%s`, rebuilding",
			 index_btree_path);
		struct vy_page_index_entry *entries = region_alloc(
			&fiber()->gc, (size_t)page_count * sizeof(*entries));
		if (entries == NULL) {
			diag_set(OutOfMemory, (size_t)page_count *
				 sizeof(*entries), "region_alloc",
				 "page index entries");
			return -1;
		}
		for (uint32_t i = 0; i < page_count; i++) {
			entries[i].idx = (int32_t)i;
			entries[i].min_key = page_info_array[i].min_key;
			entries[i].min_key_hint =
				page_info_array[i].min_key_hint;
		}
		if (vy_page_index_btree_write(entries, page_count,
					      index_btree_path,
					      &btree_root_offset,
					      &btree_data_offset,
					      page_index_cache_env) != 0)
			return -1;
	}

	/* Write .index_offsets file if not exists. */
	if (access(index_offsets_path, F_OK) != 0) {
		say_info("missing index offsets file `%s`, building",
			 index_offsets_path);
		if (vy_page_index_array_build(page_count,
					      index_path,
					      index_offsets_path,
					      page_info_cache_env) != 0)
			return -1;
	}

	vy_page_index_create(index,
			     index_path, index_btree_path, index_offsets_path,
			     page_index_cache_env, page_info_cache_env,
			     cmp_def, page_count,
			     btree_root_offset, btree_data_offset);
	if (vy_page_index_btree_open(&index->btree) != 0)
		return -1;
	if (vy_page_index_array_open(&index->page_info) != 0)
		return -1;
	return 0;
}

/* Write the page index to the .index, .btree and .offsets files. */
int
vy_page_index_write(struct vy_page_index *index,
		    struct vy_page_index_entry *entries,
		    uint32_t page_count,
		    const char *index_path,
		    const char *index_btree_path,
		    const char *index_offsets_path,
		    struct vy_page_index_cache_env *page_index_cache_env,
		    struct vy_page_info_cache_env *page_info_cache_env,
		    struct key_def *cmp_def)
{
	uint64_t btree_root_offset;
	uint64_t btree_data_offset;
	if (vy_page_index_btree_write(entries, page_count, index_btree_path,
				      &btree_root_offset, &btree_data_offset,
				      page_index_cache_env) != 0)
		return -1;
	if (vy_page_index_array_build(page_count,
				      index_path, index_offsets_path,
				      page_info_cache_env) != 0)
		return -1;
	vy_page_index_create(index,
			     index_path, index_btree_path, index_offsets_path,
			     page_index_cache_env, page_info_cache_env,
			     cmp_def, page_count,
			     btree_root_offset, btree_data_offset);
	if (vy_page_index_btree_open(&index->btree) != 0)
		return -1;
	if (vy_page_index_array_open(&index->page_info) != 0)
		return -1;
	return 0;
}

/* Find the page number for the given key. */
int
vy_page_index_find_page(struct vy_page_index *index, struct vy_entry key,
			enum iterator_type itype,
			uint32_t *result, bool *equal_key)
{
	if (itype == ITER_EQ)
		itype = ITER_GE;
	assert(itype == ITER_GE || itype == ITER_GT ||
	       itype == ITER_LE || itype == ITER_LT);

	bool lower_bound = (itype == ITER_LT || itype == ITER_GE);

	struct vy_page_index_entry prev = {0}, next = {0};
	bool hit = vy_page_index_cache_find_chain(
		&index->cache, key, lower_bound, &next, &prev, equal_key);
	/* Update cache stats. */
	if (hit)
		index->cache.env->stat.hit++;
	else
		index->cache.env->stat.miss++;
	if (hit)
		goto out;

	/* Miss. Open/preload shared state before the disk-only COIO lookup. */
	if (vy_page_index_btree_open(&index->btree) != 0)
		return -1;
	struct vy_page_index_btree_find_task task;
	memset(&task, 0, sizeof(task));
	task.btree = &index->btree;
	task.key = key;
	task.lower_bound = lower_bound;
	if (vy_run_env_coio_call(index->cache.env->run_env, &task.base,
				 vy_page_index_btree_find_cb) != 0)
		return -1;
	*equal_key = task.equal_key;
	next = task.next;
	prev = task.prev;

	if (prev.idx + 1 != next.idx) {
		vy_page_index_entry_destroy(&prev);
		vy_page_index_entry_destroy(&next);
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 index->btree.filepath, "Invalid page index chain");
		return -1;
	}

	/* Cache is checked and updated only by the caller, after COIO returns. */
	vy_page_index_cache_add_chain(&index->cache, &next, &prev);

out:
	int dir = iterator_direction(itype);
	int32_t range[2] = { prev.idx, next.idx };
	/* Only indexes are needed. */
	vy_page_index_entry_destroy(&prev);
	vy_page_index_entry_destroy(&next);
	/* Return value of invalid iterator. */
	if (range[0] < 0)
		range[0] = index->page_count;
	uint32_t page = range[dir > 0];
	/**
	 * Since page search uses only min_key of pages,
	 *  for GE, GT and EQ the previous page can contain
	 *  the point where iteration must be started.
	 */
	*result = (page > 0 && dir > 0) ? (page - 1) : page;
	return 0;
}

/* Get the page info by page number. */
int
vy_page_index_get_page(struct vy_page_index *index, uint32_t page_no,
		       struct vy_page_index_array_iterator *it)
{
	return vy_page_index_array_get_page(&index->page_info, page_no, it);
}

/* }}} Page Index */
