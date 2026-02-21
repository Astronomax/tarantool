#include "vy_page_index.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

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

struct vy_page_index_btree_path_entry {
	/* TODO: Store node in every entry to read each node only once. */
	uint64_t node_offset;
	uint32_t key_count;
	uint32_t child;
};

struct vy_page_index_btree_iterator {
	/** File offset of the node that contains the element. */
	uint64_t node_offset;
	/** Index inside node->keys[]. */
	uint32_t pos;
	/**
	 * Path from root to the current node (excluding the current node).
	 * Each entry describes from which child of the parent we descended.
	 */
	uint32_t depth;
	struct vy_page_index_btree_path_entry path[VY_PAGE_INDEX_BTREE_MAX_DEPTH];
};

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
	uint64_t *children;
	/** Offset of this node in the file. */
	uint64_t offset;
};

struct vy_page_index_entry
vy_page_index_entry_copy(struct vy_page_index_entry *entry)
{
	struct vy_page_index_entry result = *entry;
	if (entry->min_key != NULL)
		result.min_key = mp_dup(entry->min_key);
	return result;
}

struct vy_page_index_entry
vy_page_index_entry_move(struct vy_page_index_entry *entry)
{
	struct vy_page_index_entry result = *entry;
	entry->idx = 0;
	entry->min_key = NULL;
	entry->min_key_hint = HINT_NONE;
	return result;
}

void
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
	if (node->keys != NULL) {
		for (uint32_t i = 0; i < node->key_count; i++)
			vy_page_index_entry_destroy(&node->keys[i]);
		free(node->keys);
	}
	free(node->children);
	TRASH(node);
}

struct vy_page_index_btree *
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

void
vy_page_index_btree_create(struct vy_page_index_btree *btree,
			   struct key_def *cmp_def, uint64_t root_offset,
			   const char *filepath)
{
	btree->cmp_def = key_def_dup(cmp_def);
	btree->root_offset = root_offset;
	btree->data_offset = UINT64_MAX;
	btree->filepath = (char *)filepath;
	btree->fd = -1;
}

void
vy_page_index_btree_destroy(struct vy_page_index_btree *btree)
{
	if (btree->fd >= 0) {
		if (close(btree->fd) < 0)
			say_syserror("close failed");
		btree->fd = -1;
	}
	/* TODO: think about strdup. */
	TRASH(btree);
}

void
vy_page_index_btree_delete(struct vy_page_index_btree *btree)
{
	if (btree->fd >= 0) {
		if (close(btree->fd) < 0)
			say_syserror("close failed");
		btree->fd = -1;
	}
	free(btree);
}

int
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

int
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
vy_page_index_btree_ibuf_ensure(struct ibuf *buf, int fd, const char *filename,
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
		assert(node->children != NULL);
		assert(children_offset != NULL);
		*children_offset = *offset + (uint64_t)ibuf_used(wbuf);

		size_t child_count = (size_t)node->key_count + 1;
		pos = ibuf_alloc(wbuf, child_count * sizeof(uint64_t));
		if (pos == NULL) {
			diag_set(OutOfMemory, child_count * sizeof(uint64_t),
				 "ibuf_alloc", "btree node write buffer");
			return -1;
		}
		memcpy(pos, node->children, child_count * sizeof(uint64_t));
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
	    &rbuf, btree->fd, btree->filepath, &read_offset, bytes) != 0)
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
		/* entry size prefix */
		if (vy_page_index_btree_ibuf_ensure(
		    &rbuf, btree->fd, btree->filepath, &read_offset,
		    sizeof(uint32_t)) != 0)
			goto fail;
		uint32_t entry_size = *(const uint32_t *)rbuf.rpos;
		rbuf.rpos += sizeof(uint32_t);

		if (entry_size == 0) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 btree->filepath,
				 "Invalid page index entry size");
			goto fail;
		}

		if (vy_page_index_btree_ibuf_ensure(
		    &rbuf, btree->fd, btree->filepath, &read_offset,
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

	size_t child_count = (size_t)node->key_count + 1;
	bytes = child_count * sizeof(uint64_t);
	if (vy_page_index_btree_ibuf_ensure(
	    &rbuf, btree->fd, btree->filepath, &read_offset, bytes) != 0)
		goto fail;
	node->children = calloc(child_count, sizeof(uint64_t));
	if (node->children == NULL) {
		diag_set(OutOfMemory, bytes, "calloc", "btree node children");
		goto fail;
	}
	memcpy(node->children, rbuf.rpos, bytes);
	rbuf.rpos += bytes;

	ibuf_destroy(&rbuf);
	return 0;

fail:
	ibuf_destroy(&rbuf);
	vy_page_index_btree_node_destroy(node);
	return -1;
}

/* }} B Tree node serialize/deserialize */

/* {{ B Tree iterator */

static inline struct vy_page_index_btree_iterator
vy_page_index_btree_invalid_iterator(void)
{
	return (struct vy_page_index_btree_iterator){
		.node_offset = UINT64_MAX,
		.pos = 0,
		.depth = 0,
	};
}

static inline bool
vy_page_index_btree_iterator_is_invalid(struct vy_page_index_btree_iterator *it)
{
	return it->node_offset == UINT64_MAX;
}

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
	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_read(btree, &node, it->node_offset) != 0)
		return -1;
	*result = vy_page_index_entry_move(&node.keys[it->pos]);
	vy_page_index_btree_node_destroy(&node);
	return 0;
}

static inline int
vy_page_index_btree_path_push(struct vy_page_index_btree *btree,
			      struct vy_page_index_btree_iterator *it,
			      struct vy_page_index_btree_path_entry entry)
{
	if (it->depth >= VY_PAGE_INDEX_BTREE_MAX_DEPTH) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, btree->filepath,
			 "B-tree depth limit exceeded");
		return -1;
	}
	it->path[it->depth] = entry;
	++it->depth;
	return 0;
}

/**
 * Descent to the left (to the minimum) until a leaf (Finding the minimum in a
 * subtree).
 */
static int
vy_page_index_btree_iterator_descend_min(struct vy_page_index_btree *btree,
					 struct vy_page_index_btree_iterator *it,
					 uint64_t offset)
{
	while (true) {
		struct vy_page_index_btree_node node;
		if (vy_page_index_btree_node_read(btree, &node, offset) != 0)
			return -1;
		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			it->node_offset = offset;
			it->pos = 0;
			vy_page_index_btree_node_destroy(&node);
			return 0;
		}
		assert(node.children != NULL);
		uint32_t child = 0;
		uint64_t child_offset = node.children[child];
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = offset,
			.key_count = node.key_count,
			.child = child
		};
		/* The node is no longer needed. */
		vy_page_index_btree_node_destroy(&node);
		if (vy_page_index_btree_path_push(btree, it, entry) != 0)
			return -1;
		offset = child_offset;
	}
}

/**
 * Descent to the right (to the maximum) until a leaf (Finding the maximum in a
 * subtree).
 */
static int
vy_page_index_btree_iterator_descend_max(struct vy_page_index_btree *btree,
				         struct vy_page_index_btree_iterator *it,
				         uint64_t offset)
{
	while (true) {
		struct vy_page_index_btree_node node;
		if (vy_page_index_btree_node_read(btree, &node, offset) != 0)
			return -1;
		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			it->node_offset = offset;
			it->pos = node.key_count - 1;
			/* TODO: move node to iterator. */
			vy_page_index_btree_node_destroy(&node);
			return 0;
		}
		assert(node.children != NULL);
		uint32_t child = node.key_count;
		uint64_t child_offset = node.children[child];
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = offset,
			.key_count = node.key_count,
			.child = child
		};
		/* The node is no longer needed. */
		vy_page_index_btree_node_destroy(&node);
		if (vy_page_index_btree_path_push(btree, it, entry) != 0)
			return -1;
		offset = child_offset;
	}
}

static int MAYBE_UNUSED
vy_page_index_btree_iterator_next(struct vy_page_index_btree *btree,
				  struct vy_page_index_btree_iterator *it)
{
	if (vy_page_index_btree_iterator_is_invalid(it))
		return 0;

	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_read(btree, &node, it->node_offset) != 0) {
		*it = vy_page_index_btree_invalid_iterator();
		return -1;
	}

	if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		/* Go down to child. */
		uint32_t child = it->pos + 1;
		assert(child <= node.key_count);
		uint64_t child_offset = node.children[child];
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = it->node_offset,
			.key_count = node.key_count,
			.child = child
		};
		vy_page_index_btree_node_destroy(&node);
		if (vy_page_index_btree_path_push(btree, it, entry) != 0) {
			*it = vy_page_index_btree_invalid_iterator();
			return -1;
		}
		/* Descent left to next minimum >= current. */
		if (vy_page_index_btree_iterator_descend_min(
		    btree, it, child_offset) != 0) {
			*it = vy_page_index_btree_invalid_iterator();
			return -1;
		}
		return 0;
	}

	/* Leaf. */
	uint32_t key_count = node.key_count;
	vy_page_index_btree_node_destroy(&node);

	if (it->pos + 1 < key_count) {
		++it->pos;
		return 0;
	}

	/* Ascend. */
	while (it->depth > 0) {
		struct vy_page_index_btree_path_entry entry =
			it->path[it->depth - 1];
		--it->depth;
		if (entry.child < entry.key_count) {
			it->node_offset = entry.node_offset;
			it->pos = entry.child;
			return 0;
		}
	}
	*it = vy_page_index_btree_invalid_iterator();
	return 0;
}

static int
vy_page_index_btree_iterator_prev(struct vy_page_index_btree *btree,
				  struct vy_page_index_btree_iterator *it)
{
	if (vy_page_index_btree_iterator_is_invalid(it)) {
		return 0;
		///*
		// * Treat an invalid iterator as a past-the-end position:
		// * prev(end) == last element.
		// */
		//memset(it, 0, sizeof(*it));
		//return vy_page_index_btree_iterator_descend_max(
		//	btree, it, btree->root_offset);
	}

	struct vy_page_index_btree_node node;
	if (vy_page_index_btree_node_read(btree, &node, it->node_offset) != 0) {
		*it = vy_page_index_btree_invalid_iterator();
		return -1;
	}

	if (node.type == VY_PAGE_INDEX_BTREE_NODE_INTERNAL) {
		/* Go down to child. */
		uint32_t child = it->pos;
		assert(child <= node.key_count);
		uint64_t child_offset = node.children[child];
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = it->node_offset,
			.key_count = node.key_count,
			.child = child
		};
		vy_page_index_btree_node_destroy(&node);
		if (vy_page_index_btree_path_push(btree, it, entry) != 0) {
			*it = vy_page_index_btree_invalid_iterator();
			return -1;
		}
		/* Descent right to next maximum <= current. */
		if (vy_page_index_btree_iterator_descend_max(
		    btree, it, child_offset) != 0) {
			*it = vy_page_index_btree_invalid_iterator();
			return -1;
		}
		return 0;
	}

	/* Leaf. */
	vy_page_index_btree_node_destroy(&node);

	if (it->pos > 0) {
		--it->pos;
		return 0;
	}

	/* Ascend. */
	while (it->depth > 0) {
		struct vy_page_index_btree_path_entry entry =
			it->path[it->depth - 1];
		--it->depth;
		if (entry.child > 0) {
			it->node_offset = entry.node_offset;
			it->pos = entry.child - 1;
			return 0;
		}
	}
	*it = vy_page_index_btree_invalid_iterator();
	return 0;
}

/* }} B Tree iterator */

/* {{ B Tree lower/upper_bound */

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

	uint64_t offset = btree->root_offset;
	struct vy_page_index_btree_iterator it = {
		.node_offset = offset,
		.pos = 0,
		.depth = 0
	};

	/* Descend. */
	while (true) {
		struct vy_page_index_btree_node node;
		if (vy_page_index_btree_node_read(btree, &node, offset) != 0) {
			*result = vy_page_index_btree_invalid_iterator();
			return -1;
		}

		bool eq = false;
		uint32_t child = vy_page_index_btree_node_lower_bound(
			btree, &node, key, &eq);
		*equal_key = *equal_key || eq;

		if (child < node.key_count) {
			result->depth = it.depth;
			/* TODO: think about copy here. */
			memcpy(result->path, it.path,
			       (size_t)it.depth * sizeof(it.path[0]));
			/*
			 * In `it` we use only depth and path, node_offset and
			 * pos are invalid.
			 */
			result->node_offset = offset;
			result->pos = child;
		}

		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			vy_page_index_btree_node_destroy(&node);
			return 0;
		}
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = offset,
			.key_count = node.key_count,
			.child = child
		};
		assert(node.children != NULL);
		offset = node.children[child];
		vy_page_index_btree_node_destroy(&node);

		if (vy_page_index_btree_path_push(btree, &it, entry) != 0) {
			*result = vy_page_index_btree_invalid_iterator();
			return -1;
		}
	}
}

static int
vy_page_index_btree_upper_bound(struct vy_page_index_btree *btree,
				struct vy_entry key,
				struct vy_page_index_btree_iterator *result,
				bool *equal_key)
{
	*equal_key = false;
	*result = vy_page_index_btree_invalid_iterator();

	uint64_t offset = btree->root_offset;
	struct vy_page_index_btree_iterator it = {
		.node_offset = offset,
		.pos = 0,
		.depth = 0
	};

	while (true) {
		struct vy_page_index_btree_node node;
		if (vy_page_index_btree_node_read(btree, &node, offset) != 0) {
			*result = vy_page_index_btree_invalid_iterator();
			return -1;
		}

		bool eq = false;
		/* The only difference is we are using upper instead of lower. */
		uint32_t child = vy_page_index_btree_node_upper_bound(
			btree, &node, key, &eq);
		*equal_key = *equal_key || eq;

		if (child < node.key_count) {
			result->depth = it.depth;
			memcpy(result->path, it.path,
			       (size_t)it.depth * sizeof(it.path[0]));
			result->node_offset = offset;
			result->pos = child;
		}

		if (node.type == VY_PAGE_INDEX_BTREE_NODE_LEAF) {
			vy_page_index_btree_node_destroy(&node);
			return 0;
		}
		struct vy_page_index_btree_path_entry entry = {
			.node_offset = offset,
			.key_count = node.key_count,
			.child = child
		};
		assert(node.children != NULL);
		offset = node.children[child];
		vy_page_index_btree_node_destroy(&node);

		if (vy_page_index_btree_path_push(btree, &it, entry) != 0) {
			*result = vy_page_index_btree_invalid_iterator();
			return -1;
		}
	}
}

static int
vy_page_index_btree_last(struct vy_page_index_btree *btree,
			 struct vy_page_index_btree_iterator *it)
{
	memset(it, 0, sizeof(*it));
	return vy_page_index_btree_iterator_descend_max(
		btree, it, btree->root_offset);
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
				struct key_def *cmp_def,
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
	node.children = zero_children;

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
		    cmp_def, wbuf, offset, &children[i]) != 0) {
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
int
vy_page_index_btree_build(struct vy_page_index_btree *btree,
			  struct vy_page_index_entry *pages, uint32_t page_count,
			  struct key_def *cmp_def, const char *filepath)
{
	assert(page_count > 0);
	struct ibuf wbuf;
	ibuf_create(&wbuf, &cord()->slabc, 4096);

	uint64_t root_offset = 0;
	uint64_t written = 0;
	if (vy_page_index_btree_build_range(pages, 0, page_count, cmp_def,
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

	btree->cmp_def = cmp_def;
	btree->root_offset = root_offset;
	btree->data_offset = UINT64_MAX;
	ibuf_destroy(&wbuf);
	return 0;

fail_discard_xlog:
	xlog_discard(&xlog);
fail:
	ibuf_destroy(&wbuf);
	return -1;
}

/* }} B Tree build */

/* }}} B Tree */

/* {{{ Cache */

static void *
vy_page_index_cache_tree_page_alloc(struct matras_allocator *allocator)
{
	(void)allocator;
	return xmalloc(VY_PAGE_INDEX_CACHE_TREE_EXTENT_SIZE);
}

static void
vy_page_index_cache_tree_page_free(struct matras_allocator *allocator, void *ptr)
{
	(void)allocator;
	free(ptr);
}

void
vy_page_index_cache_env_create(struct vy_page_index_cache_env *env,
			       struct slab_cache *slab_cache)
{
	rlist_create(&env->cache_lru);
	env->mem_used = 0;
	env->mem_quota = 10000;
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
	return sizeof(*node); /* TODO: + ??? */
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
	env->mem_used -= vy_page_index_cache_node_size(node);
	rlist_del_entry(node, in_lru);
	vy_page_index_entry_destroy(&node->entry);
	mempool_free(&env->cache_node_mempool, node);
}

static inline void
vy_page_index_cache_touch(struct vy_page_index_cache_node *node)
{
	rlist_move_entry(&node->cache->env->cache_lru, node, in_lru);
}

void
vy_page_index_cache_create(struct vy_page_index_cache *cache,
			   struct vy_page_index_cache_env *env,
			   struct key_def *cmp_def)
{
	memset(cache, 0, sizeof(*cache));
	cache->cmp_def = key_def_dup(cmp_def);
	cache->env = env;
	matras_stats_create(&cache->matras_stats);
	vy_page_index_cache_tree_create(&cache->cache_tree, cache->cmp_def,
					&env->allocator, &cache->matras_stats);
}

void
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

void
vy_page_index_create(struct vy_page_index *index,
		     struct vy_page_index_cache_env *env,
		     struct key_def *cmp_def, uint64_t root_offset,
		     const char *filepath, uint32_t page_count)
{
	index->page_count = page_count;
	vy_page_index_cache_create(&index->cache, env, cmp_def);
	index->cache.page_count = page_count;
	vy_page_index_btree_create(&index->btree, cmp_def, root_offset, filepath);
	index->btree.page_count = page_count;
}

void
vy_page_index_destroy(struct vy_page_index *index)
{
	vy_page_index_cache_destroy(&index->cache);
	vy_page_index_btree_destroy(&index->btree);
	TRASH(index);
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

void
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
void
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

		int cmp = vy_page_index_entry_compare(prev_check, prev,
						      cache->cmp_def);
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
	replaced = NULL;
	struct vy_page_index_cache_node *successor = NULL;
	if (vy_page_index_cache_tree_insert(
		&cache->cache_tree, prev_node, &replaced, &successor) != 0) {
		/* memory error, let's live without a cache */
		vy_page_index_cache_node_delete(cache->env, prev_node);
		return;
	}
	assert(replaced == NULL);
	assert(successor == next_node);
}

bool
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

/* }}} Cache */

/* {{{ Page Index */

int
vy_page_index_btree_find_chain(struct vy_page_index_btree *btree,
			       struct vy_entry key, bool lower_bound,
			       struct vy_page_index_entry *next,
			       struct vy_page_index_entry *prev,
			       bool *equal_key)
{
	if (vy_page_index_btree_open(btree) != 0)
		return -1;

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
	return 0;
fail:
	return -1;
}

int
vy_page_index_find_page(struct vy_page_index *index, struct vy_entry key,
			enum iterator_type itype,
			uint32_t *result, bool *equal_key)
{
	fprintf(stderr, "vy_page_index_find_page: %u\n", index->page_count);
	fflush(stderr);

	if (itype == ITER_EQ)
		itype = ITER_GE;
	assert(itype == ITER_GE || itype == ITER_GT ||
	       itype == ITER_LE || itype == ITER_LT);

	bool lower_bound = (itype == ITER_LT || itype == ITER_GE);

	struct vy_page_index_entry prev = {0}, next = {0};
	bool hit = vy_page_index_cache_find_chain(
		&index->cache, key, lower_bound, &next, &prev, equal_key);
	if (hit) {
		fprintf(stderr, "cache hit\n");
		fflush(stderr);
		goto out;
	}

	fprintf(stderr, "cache miss\n");
	fflush(stderr);

	/* Miss. Go to disk. */
	if (vy_page_index_btree_find_chain(
	    &index->btree, key, lower_bound, &next, &prev, equal_key) != 0)
		return -1;

	assert(prev.idx + 1 == next.idx);
	/* Add chain to cache. */
	fprintf(stderr, "prev: %d, next: %d\n", prev.idx, next.idx);
	fflush(stderr);
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

/* }}} Page Index */

/* TODO: support variable chain length. */
