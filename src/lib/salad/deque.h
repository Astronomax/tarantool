#ifndef TARANTOOL_LIB_SALAD_DEQUE_H_INCLUDED
#define TARANTOOL_LIB_SALAD_DEQUE_H_INCLUDED
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
#include <stdlib.h>

#ifndef _DEQUE_BUF_SIZE
#define _DEQUE_BUF_SIZE 512
#endif

inline size_t
__deque_buf_size(size_t __size)
{ return (__size < _DEQUE_BUF_SIZE
	  ? (size_t)(_DEQUE_BUF_SIZE / __size) : (size_t)(1)); }

struct deque_iterator {
    deque_value_t *_M_cur;
    deque_value_t *_M_first;
    deque_value_t *_M_last;
    void **_M_node;
};

void
_M_set_node(struct deque_iterator *it, void **__new_node)
{
	it->_M_node = __new_node;
	it->_M_first = *__new_node;
	it->_M_last = it->_M_first + _DEQUE_BUF_SIZE;
}

static inline deque_value_t *
deque_iterator_next(struct deque_iterator *it)
{
	++it->_M_cur;
	if (it->_M_cur == it->_M_last) {
		_M_set_node(it, it->_M_node + 1);
		it->_M_cur = it->_M_first;
	}
	return ...;
}

static inline deque_value_t *
deque_iterator_prev(struct deque_iterator *it)
{
	if (it->_M_cur == it->_M_first)
	{
		_M_set_node(it, it->_M_node - 1);
		it->_M_cur = it->_M_last;
	}
	--it->_M_cur;
	return ...;
}

struct deque {

    struct deque_iterator _M_start;
    struct deque_iterator _M_finish;
};

void
_M_pop_front_aux(struct deque *d)
{
	//_Alloc_traits::destroy(_M_get_Tp_allocator(),
	//		       this->_M_impl._M_start._M_cur);
	//_M_deallocate_node(this->_M_impl._M_start._M_first);
	_M_set_node(&d->_M_start, d->_M_start._M_node + 1);
	d->_M_start._M_cur = d->_M_start._M_first;
}

void
_M_pop_back_aux(struct deque *d)
{
	//_M_deallocate_node(this->_M_impl._M_finish._M_first);
	_M_set_node(d->_M_finish, d->_M_finish._M_node - 1);
	d->_M_finish._M_cur = d->_M_finish._M_last - 1;
	//_Alloc_traits::destroy(_M_get_Tp_allocator(),
	//		       this->_M_impl._M_finish._M_cur);
}

int
_M_push_front_aux(struct deque *d, const deque_value_t& __t)
{
if (size() == max_size())
return -1;
//__throw_length_error(
//	__N("cannot create std::deque larger than max_size()"));

_M_reserve_map_at_front();
*(this->_M_impl._M_start._M_node - 1) = this->_M_allocate_node();
__try {
this->_M_impl._M_start._M_set_node(this->_M_impl._M_start._M_node - 1);
this->_M_impl._M_start._M_cur = this->_M_impl._M_start._M_last - 1;
this->_M_impl.construct(this->_M_impl._M_start._M_cur, __t);
} __catch(...) {
++this->_M_impl._M_start;
_M_deallocate_node(*(this->_M_impl._M_start._M_node - 1));
__throw_exception_again;
}
}

void
_M_push_back_aux(struct deque *d, const deque_value_t& __t)
{
if (size() == max_size())
return -1;
//__throw_length_error(
//	__N("cannot create std::deque larger than max_size()"));

_M_reserve_map_at_back();
*(this->_M_impl._M_finish._M_node + 1) = this->_M_allocate_node();
__try {
this->_M_impl.construct(this->_M_impl._M_finish._M_cur, __t);
this->_M_impl._M_finish._M_set_node(this->_M_impl._M_finish._M_node + 1);
this->_M_impl._M_finish._M_cur = this->_M_impl._M_finish._M_first;
} __catch(...) {
_M_deallocate_node(*(this->_M_impl._M_finish._M_node + 1));
__throw_exception_again;
}
}

void
deque_pop_front(struct deque *d)
{
	if (d->_M_start._M_cur != d->_M_start._M_last - 1) {
		//_Alloc_traits::destroy(_M_get_Tp_allocator(),
		// 			this->_M_impl._M_start._M_cur);
		++d->_M_start._M_cur;
	} else {
		_M_pop_front_aux(d);
	}
}

void
deque_pop_back(struct deque *d)
{
	if (d->_M_finish._M_cur != d->_M_finish._M_first) {
		--d->_M_finish._M_cur;
		//_Alloc_traits::destroy(_M_get_Tp_allocator(),
		//this->_M_impl._M_finish._M_cur);
	} else {
		_M_pop_back_aux(d);
	}
}

void
deque_push_front(struct deque *d, const deque_value_t& __x)
{
if (d->_M_start._M_cur != d->_M_start._M_first) {
//_Alloc_traits::construct(this->_M_impl,
//			 this->_M_impl._M_start._M_cur - 1,
//			 __x);
--d->_M_start._M_cur;
} else {
_M_push_front_aux(d, __x);
}
}

void
deque_push_back(struct deque *d, const deque_value_t& __x)
{
if (d->_M_finish._M_cur != d->_M_finish._M_last - 1) {
//_Alloc_traits::construct(this->_M_impl,
//			 this->_M_impl._M_finish._M_cur, __x);
++d->_M_finish._M_cur;
} else {
_M_push_back_aux(d, __x);
}
}

void
deque_create(struct deque *d, size_t num_elements)
{
	const size_t __num_nodes = (num_elements / __deque_buf_size(sizeof(deque_value_t)) + 1);

	fifo_create(&d->fifo, __num_nodes);

	this->_M_impl._M_map_size = std::max((size_t) _S_initial_map_size,
					     size_t(__num_nodes + 2));
	this->_M_impl._M_map = _M_allocate_map(this->_M_impl._M_map_size);

	// For "small" maps (needing less than _M_map_size nodes), allocation
	// starts in the middle elements and grows outwards.  So nstart may be
	// the beginning of _M_map, but for small maps it may be as far in as
	// _M_map+3.

	_Map_pointer __nstart = (this->_M_impl._M_map
				 + (this->_M_impl._M_map_size - __num_nodes) / 2);
	_Map_pointer __nfinish = __nstart + __num_nodes;

	__try
	{ _M_create_nodes(__nstart, __nfinish); }
	__catch(...)
	{
		_M_deallocate_map(this->_M_impl._M_map, this->_M_impl._M_map_size);
		this->_M_impl._M_map = _Map_pointer();
		this->_M_impl._M_map_size = 0;
		__throw_exception_again;
	}

	this->_M_impl._M_start._M_set_node(__nstart);
	this->_M_impl._M_finish._M_set_node(__nfinish - 1);
	this->_M_impl._M_start._M_cur = _M_impl._M_start._M_first;
	this->_M_impl._M_finish._M_cur = (this->_M_impl._M_finish._M_first
					  + __num_elements
					    % __deque_buf_size(sizeof(_Tp)));
}

static inline void
deque_destroy(struct fifo *q)
{

}

#endif /* TARANTOOL_LIB_SALAD_DEQUE_H_INCLUDED */
