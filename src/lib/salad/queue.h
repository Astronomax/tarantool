/*
 * Copyright 2010-2024, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdbool.h>
#include "fifo.h"

/* For predefinition of structures and type non specific functions just make:
 * #define QUEUE_FORWARD_DECLARATION
 * #inlude "queue.h"
 */
#ifndef QUEUE_FORWARD_DECLARATION

#ifndef QUEUE_NAME
#error "QUEUE_NAME must be defined"
#endif /* QUEUE_NAME */

/** Structure, stored in the queue. */
#ifndef queue_value_t
#error "queue_value_t must be defined"
#endif

typedef queue_value_t* _Elt_pointer;

/**
 * Tools for name substitution:
 */
#ifndef CONCAT3
#define CONCAT3_R(a, b, c) a##b##c
#define CONCAT3(a, b, c) CONCAT3_R(a, b, c)
#endif

#ifdef _
#error '_' must be undefinded!
#endif
#ifndef QUEUE
#define QUEUE(name) CONCAT3(QUEUE_NAME, _, name)
#endif

#ifndef _QUEUE_BUF_SIZE
#define _QUEUE_BUF_SIZE 512
#endif

static const size_t QUEUE(BUF_SIZE) = (sizeof(queue_value_t) < _QUEUE_BUF_SIZE
				       ? (size_t)(_QUEUE_BUF_SIZE / sizeof(queue_value_t)) : (size_t)(1));

#endif /* QUEUE_FORWARD_DECLARATION */

/* Structures. */

#ifndef QUEUE_STRUCTURES /* Include guard for structures */

#define QUEUE_STRUCTURES

typedef void** _Map_pointer;

/**
 * Type of the node allocator (the allocator for regions
 * of size _QUEUE_BUF_SIZE). Is allowed to return NULL, but is not allowed
 * to throw an exception
 */
typedef void *(*queue_alloc_func)(void *ctx);
typedef void (*queue_free_func)(void *ctx, void *ptr);

struct queue_iterator {
    void* _M_cur;
    void* _M_first;
    void* _M_last;
    void* _M_node;
};

struct queue {
    struct fifo fifo;
    struct queue_iterator _M_start;
    struct queue_iterator _M_finish;
    /* External extent allocator */
    queue_alloc_func alloc_func;
    /* External extent deallocator */
    queue_free_func free_func;
    /* Argument passed to extent allocator */
    void *alloc_ctx;
};

#endif /* QUEUE_STRUCTURES */

#ifndef QUEUE_FORWARD_DECLARATION

static inline void *
QUEUE(_M_allocate_node)(struct queue *q)
{
	return q->alloc_func(q->alloc_ctx);
}

static inline void
QUEUE(_M_deallocate_node)(struct queue *q, void *__p)
{
	q->free_func(q->alloc_ctx, __p);
}

static inline void
QUEUE(_M_set_node)(struct queue_iterator *it, _Map_pointer __new_node)
{
	it->_M_node = (void*)__new_node;
	it->_M_first = *__new_node;
	it->_M_last = (void*)((_Elt_pointer)it->_M_first + QUEUE(BUF_SIZE));
}

static inline bool
QUEUE(iterator_is_equal)(struct queue_iterator *it1, struct queue_iterator *it2)
{
	return (it1->_M_node == it2->_M_node && it1->_M_cur == it2->_M_cur);
}

static inline _Elt_pointer
QUEUE(iterator_unref)(struct queue_iterator *it)
{
	return (_Elt_pointer)it->_M_cur;
}

struct queue_iterator
QUEUE(begin)(struct queue *q)
{
	return q->_M_start;
}

struct queue_iterator
QUEUE(end)(struct queue *q)
{
	return q->_M_finish;
}

static inline void
QUEUE(iterator_next)(struct queue_iterator *it)
{
	it->_M_cur = (void*)((_Elt_pointer)it->_M_cur + 1);
	if (it->_M_cur == it->_M_last) {
		QUEUE(_M_set_node)(it, (_Map_pointer)it->_M_node + 1);
		it->_M_cur = it->_M_first;
	}
}

static inline void
QUEUE(iterator_prev)(struct queue_iterator *it)
{
	if (it->_M_cur == it->_M_first) {
		QUEUE(_M_set_node)(it, (_Map_pointer)it->_M_node - 1);
		it->_M_cur = it->_M_last;
	}
	it->_M_cur = (void*)((_Elt_pointer)it->_M_cur - 1);
}

static inline void
QUEUE(iterator_shift)(struct queue_iterator *it, long __n)
{
	const long __offset = __n + ((_Elt_pointer)it->_M_cur
				     - (_Elt_pointer)it->_M_first);
	if (__offset >= 0 && __offset < (long)QUEUE(BUF_SIZE))
		it->_M_cur = (void*)((_Elt_pointer)it->_M_cur + __n);
	else
	{
		const long __node_offset = __offset > 0 ?
					   __offset / (long)QUEUE(BUF_SIZE) :
					   -(long)(-__offset - 1) / (long)QUEUE(BUF_SIZE) - 1;
		QUEUE(_M_set_node)(it,(_Map_pointer)it->_M_node + __node_offset);
		it->_M_cur = (void*)((_Elt_pointer)it->_M_first +
				     (__offset - __node_offset * QUEUE(BUF_SIZE)));
	}
}

void
QUEUE(_M_pop_front_aux)(struct queue *q)
{
	QUEUE(_M_deallocate_node)(q, q->_M_start._M_first);
	fifo_pop(&q->fifo);
	QUEUE(_M_set_node)(&q->_M_start, (_Map_pointer)q->_M_start._M_node + 1);
	q->_M_start._M_cur = q->_M_start._M_first;
}

_Elt_pointer
QUEUE(_M_push_back_aux)(struct queue *q)
{
	void *__node = QUEUE(_M_allocate_node)(q);
	if (__node == NULL)
		return NULL;
	if (fifo_push(&q->fifo, __node) == -1) {
		QUEUE(_M_deallocate_node)(q, __node);
		return NULL;
	}
	_Elt_pointer ret = (_Elt_pointer)q->_M_finish._M_cur;
	q->_M_start._M_node = (void*)(q->fifo.buf + q->fifo.bottom);
	QUEUE(_M_set_node)(&q->_M_finish, (_Map_pointer)(q->fifo.buf + q->fifo.top) - 1);
	q->_M_finish._M_cur = q->_M_finish._M_first;
	return ret;
}

static inline bool
QUEUE(empty)(struct queue *q)
{
	return QUEUE(iterator_is_equal)(&q->_M_start, &q->_M_finish);
}

static inline _Elt_pointer
QUEUE(at)(struct queue *q, size_t __n)
{
	struct queue_iterator it = q->_M_start;
	QUEUE(iterator_shift)(&it, (long)__n);
	return QUEUE(iterator_unref)(&it);
}

static inline void
QUEUE(pop)(struct queue *q)
{
	if ((_Elt_pointer)q->_M_start._M_cur != (_Elt_pointer)q->_M_start._M_last - 1)
		q->_M_start._M_cur = (void*)((_Elt_pointer)q->_M_start._M_cur + 1);
	else
		QUEUE(_M_pop_front_aux)(q);
}

static inline _Elt_pointer
QUEUE(push)(struct queue *q)
{
	if ((_Elt_pointer)q->_M_finish._M_cur != (_Elt_pointer)q->_M_finish._M_last - 1) {
		_Elt_pointer ret = (_Elt_pointer)q->_M_finish._M_cur;
		q->_M_finish._M_cur = (void*)((_Elt_pointer)q->_M_finish._M_cur + 1);
		return ret;
	} else
		return QUEUE(_M_push_back_aux)(q);
}

static inline void
QUEUE(_M_destroy_nodes)(struct queue *q, _Map_pointer __nstart, _Map_pointer __nfinish)
{
	for (_Map_pointer __n = __nstart; __n < __nfinish; ++__n)
		QUEUE(_M_deallocate_node)(q, *__n);
}

static inline int
QUEUE(_M_create_nodes)(struct queue *q, _Map_pointer __nstart, _Map_pointer __nfinish)
{
	_Map_pointer __cur;
	for (__cur = __nstart; __cur < __nfinish; ++__cur) {
		*__cur = QUEUE(_M_allocate_node)(q);
		if (*__cur == NULL) {
			QUEUE(_M_destroy_nodes)(q, __nstart, __cur);
			return -1;
		}
	}
	return 0;
}

/**
 * Init current queue.
 */
static inline int
QUEUE(create)(struct queue *q, size_t num_elements, queue_alloc_func alloc_func,
	      queue_free_func free_func, void *alloc_ctx)
{
	q->alloc_func = alloc_func;
	q->free_func = free_func;
	q->alloc_ctx = alloc_ctx;

	const size_t __num_nodes = (num_elements / QUEUE(BUF_SIZE) + 1);

	fifo_create(&q->fifo, __num_nodes * sizeof(void*));
	for(size_t i = 0; i < __num_nodes; i++)
		fifo_push(&q->fifo, NULL);

	_Map_pointer __nstart = (_Map_pointer)(q->fifo.buf + q->fifo.bottom);
	_Map_pointer __nfinish = (_Map_pointer)(q->fifo.buf + q->fifo.top);

	if (QUEUE(_M_create_nodes)(q, __nstart, __nfinish) == -1) {
		fifo_destroy(&q->fifo);
		return -1;
	}

	QUEUE(_M_set_node)(&q->_M_start, __nstart);
	QUEUE(_M_set_node)(&q->_M_finish, __nfinish - 1);
	q->_M_start._M_cur = (void*)q->_M_start._M_first;
	q->_M_finish._M_cur = (void*)(q->_M_finish._M_first
				      + num_elements % QUEUE(BUF_SIZE));
	return 0;
}

/**
 * Destroy current queue.
 */
static inline void
QUEUE(destroy)(struct queue *q)
{
	QUEUE(_M_destroy_nodes)(q, (_Map_pointer)q->_M_start._M_node,
				(_Map_pointer)q->_M_finish._M_node + 1);
	fifo_destroy(&q->fifo);
}

#undef queue_value_t
#undef _QUEUE_BUF_SIZE

#endif /* QUEUE_FORWARD_DECLARATION */

#undef QUEUE_FORWARD_DECLARATION
#undef QUEUE_NAME
