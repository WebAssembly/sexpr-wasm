/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stack-allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRACE_ALLOCATOR 0

#if TRACE_ALLOCATOR
#define TRACEF(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACEF(...)
#endif

#if WABT_STACK_ALLOCATOR_STATS
#include <stdio.h>
#endif /* WABT_STACK_ALLOCATOR_STATS */

#define CHUNK_ALIGN 8
#define CHUNK_SIZE (1024 * 1024)
#define CHUNK_MAX_AVAIL (CHUNK_SIZE - sizeof(WabtStackAllocatorChunk))

typedef struct StackAllocatorMark {
  WabtStackAllocatorChunk* chunk;
  void* chunk_current;
  void* last_allocation;
} StackAllocatorMark;

#ifndef NDEBUG
static WabtBool is_power_of_two(uint32_t x) {
  return x && ((x & (x - 1)) == 0);
}
#endif /* NDEBUG */

static void* align_up(void* p, size_t align) {
  return (void*)(((intptr_t)p + align - 1) & ~(align - 1));
}

static WabtBool allocation_in_chunk(WabtStackAllocatorChunk* chunk, void* p) {
  return p >= (void*)chunk && p < chunk->end;
}

static WabtStackAllocatorChunk* allocate_chunk(
    WabtStackAllocator* stack_allocator,
    size_t max_avail,
    const char* file,
    int line) {
  assert(max_avail < SIZE_MAX - sizeof(WabtStackAllocatorChunk));
  size_t real_size = max_avail + sizeof(WabtStackAllocatorChunk);
  /* common case of allocating a chunk of exactly CHUNK_SIZE */
  if (real_size == CHUNK_SIZE) {
    if (stack_allocator->next_free) {
      WabtStackAllocatorChunk* chunk = stack_allocator->next_free;
      stack_allocator->next_free = stack_allocator->next_free->next_free;
      return chunk;
    }
  }

  WabtStackAllocatorChunk* chunk =
      wabt_alloc(stack_allocator->fallback, real_size, CHUNK_ALIGN);
  if (!chunk) {
    if (stack_allocator->has_jmpbuf)
      longjmp(stack_allocator->jmpbuf, 1);
    WABT_FATAL("%s:%d: memory allocation failed\n", file, line);
  }
  /* use the same allocation for the WabtStackAllocatorChunk and its data. + 1
   * skips over the WabtStackAllocatorChunk */
  chunk->start = chunk + 1;
  chunk->current = chunk->start;
  chunk->end = (void*)((intptr_t)chunk->start + max_avail);
  chunk->prev = NULL;
#if WABT_STACK_ALLOCATOR_STATS
  stack_allocator->chunk_alloc_count++;
  stack_allocator->total_chunk_bytes += CHUNK_SIZE;
#endif /* WABT_STACK_ALLOCATOR_STATS */
  return chunk;
}

static void* stack_alloc(WabtAllocator* allocator,
                         size_t size,
                         size_t align,
                         const char* file,
                         int line) {
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  assert(is_power_of_two(align));
  WabtStackAllocatorChunk* chunk = stack_allocator->last;
  assert(size < SIZE_MAX - align + 1);
  size_t alloc_size = size + align - 1;
  void* result;
  if (alloc_size >= CHUNK_MAX_AVAIL) {
    chunk = allocate_chunk(stack_allocator, alloc_size, file, line);
    result = align_up(chunk->current, align);
    assert((void*)((intptr_t)result + size) <= chunk->end);
    chunk->current = chunk->end;
    /* thread this chunk before first. There's no available space, so it's not
     worth considering. */
    stack_allocator->first->prev = chunk;
    stack_allocator->first = chunk;
  } else {
    assert(chunk);

    chunk->current = align_up(chunk->current, align);
    void* new_current = (void*)((intptr_t)chunk->current + size);
    if (new_current < chunk->end) {
      result = chunk->current;
      chunk->current = new_current;
    } else {
      chunk = allocate_chunk(stack_allocator, CHUNK_MAX_AVAIL, file, line);
      chunk->prev = stack_allocator->last;
      stack_allocator->last = chunk;
      chunk->current = align_up(chunk->current, align);
      result = chunk->current;
      chunk->current = (void*)((intptr_t)chunk->current + size);
      assert(chunk->current <= chunk->end);
    }

    stack_allocator->last_allocation = result;
  }

#if TRACE_ALLOCATOR
  if (file) {
    TRACEF("%s:%d: stack_alloc(%" PRIzd ", align=%" PRIzd ") => %p\n", file,
           line, size, align, result);
  }
#endif /* TRACE_ALLOCATOR */

#if WABT_STACK_ALLOCATOR_STATS
  stack_allocator->alloc_count++;
  stack_allocator->total_alloc_bytes += size;
#endif /* WABT_STACK_ALLOCATOR_STATS */
  return result;
}

static void* stack_realloc(WabtAllocator* allocator,
                           void* p,
                           size_t size,
                           size_t align,
                           const char* file,
                           int line) {
  if (!p)
    return stack_alloc(allocator, size, align, file, line);

  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  /* TODO(binji): optimize */
  WabtStackAllocatorChunk* chunk = stack_allocator->last;
  while (chunk) {
    if (allocation_in_chunk(chunk, p))
      break;
    chunk = chunk->prev;
  }

  assert(chunk);
  void* result = stack_alloc(allocator, size, align, NULL, 0);

#if TRACE_ALLOCATOR
  if (file) {
    TRACEF("%s:%d: stack_realloc(%p, %" PRIzd ", align=%" PRIzd ") => %p\n",
           file, line, p, size, align, result);
  }
#endif /* TRACE_ALLOCATOR */

  /* We know that the previously allocated data was at most extending from |p|
   to the end of the chunk. So we can copy at most that many bytes, or the
   new size, whichever is less. Use memmove because the regions may be
   overlapping. */
  size_t old_max_size = (size_t)chunk->end - (size_t)p;
  size_t copy_size = size > old_max_size ? old_max_size : size;
  memmove(result, p, copy_size);
#if WABT_STACK_ALLOCATOR_STATS
  /* count this is as a realloc, not an alloc */
  stack_allocator->alloc_count--;
  stack_allocator->realloc_count++;
  stack_allocator->total_alloc_bytes -= size;
  stack_allocator->total_realloc_bytes += size;
#endif /* WABT_STACK_ALLOCATOR_STATS */
  return result;
}

static void stack_free(WabtAllocator* allocator,
                       void* p,
                       const char* file,
                       int line) {
  if (!p)
    return;

  TRACEF("%s:%d: stack_free(%p)\n", file, line, p);
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;

#if WABT_STACK_ALLOCATOR_STATS
  stack_allocator->free_count++;
#endif /* WABT_STACK_ALLOCATOR_STATS */

  if (p != stack_allocator->last_allocation)
    return;

  WabtStackAllocatorChunk* chunk = stack_allocator->last;
  assert(allocation_in_chunk(chunk, p));
  chunk->current = p;
}

static void stack_destroy(WabtAllocator* allocator) {
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  /* destroy the free chunks */
  WabtStackAllocatorChunk* chunk = stack_allocator->next_free;
  while (chunk) {
    WabtStackAllocatorChunk* next_free = chunk->next_free;
    wabt_free(stack_allocator->fallback, chunk);
    chunk = next_free;
  }

  /* destroy the used chunks */
  chunk = stack_allocator->last;
  while (chunk) {
    WabtStackAllocatorChunk* prev = chunk->prev;
    wabt_free(stack_allocator->fallback, chunk);
    chunk = prev;
  }
}

static WabtAllocatorMark stack_mark(WabtAllocator* allocator) {
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;

  /* allocate the space for the mark, but copy the current stack state now, so
   * when we reset we reset before the mark was allocated */
  StackAllocatorMark mark;
  mark.chunk = stack_allocator->last;
  mark.chunk_current = mark.chunk->current;
  mark.last_allocation = stack_allocator->last_allocation;

  StackAllocatorMark* allocated_mark = stack_alloc(
      allocator, sizeof(StackAllocatorMark), WABT_DEFAULT_ALIGN, NULL, 0);
#if WABT_STACK_ALLOCATOR_STATS
  /* don't count this allocation */
  stack_allocator->alloc_count--;
  stack_allocator->total_alloc_bytes -= sizeof(StackAllocatorMark);
#endif /* WABT_STACK_ALLOCATOR_STATS */

  *allocated_mark = mark;
  return allocated_mark;
}

static void stack_reset_to_mark(WabtAllocator* allocator,
                                WabtAllocatorMark mark) {
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  StackAllocatorMark* stack_mark = (StackAllocatorMark*)mark;
  WabtStackAllocatorChunk* chunk = stack_allocator->last;
  while (chunk && chunk != stack_mark->chunk) {
    WabtStackAllocatorChunk* prev = chunk->prev;
    /* reset this chunk for future use, and thread it into the free list */
    chunk->current = chunk->start;
    chunk->next_free = stack_allocator->next_free;
    stack_allocator->next_free = chunk;
    chunk = prev;
  }

  assert(chunk == stack_mark->chunk);
  stack_allocator->last = chunk;
  chunk->current = stack_mark->chunk_current;
  stack_allocator->last_allocation = stack_mark->last_allocation;
}

#if WABT_STACK_ALLOCATOR_STATS
static const char* human_readable(size_t count) {
  /* printing a size_t in decimal requires ceil(sizeof(size_t) * 8 / log2(10))
   * characters. We want to add one comma for each group of three digits, so
   * the total number of characters needed is 4/3 multiplied by that value. For
   * a 64-bit size_t, this comes out to 27 (including the null-terminator). */
  static char buffer[27];
  int next_comma_in = 3;
  memset(buffer, ' ', sizeof(buffer));
  char* p = &buffer[sizeof(buffer) - 1];
  *p-- = '\0';
  if (count) {
    while (count) {
      if (next_comma_in == 0) {
        *p-- = ',';
        next_comma_in = 3;
      }
      assert(p >= buffer);
      size_t digit = count % 10;
      *p-- = '0' + digit;
      count /= 10;
      next_comma_in--;
    }
  } else {
    *p-- = '0';
  }
  return buffer;
}
#endif /* WABT_STACK_ALLOCATOR_STATS */

static void stack_print_stats(WabtAllocator* allocator) {
#if WABT_STACK_ALLOCATOR_STATS
#define VALUE(name) human_readable(stack_allocator->name)
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  printf("STACK ALLOCATOR STATS:\n");
  printf("===============================================\n");
  printf("chunk_alloc_count:   %s\n", VALUE(chunk_alloc_count));
  printf("alloc_count:         %s\n", VALUE(alloc_count));
  printf("realloc_count:       %s\n", VALUE(realloc_count));
  printf("free_count:          %s\n", VALUE(free_count));
  printf("total_chunk_bytes:   %s\n", VALUE(total_chunk_bytes));
  printf("total_alloc_bytes:   %s\n", VALUE(total_alloc_bytes));
  printf("total_realloc_bytes: %s\n", VALUE(total_realloc_bytes));
#undef VALUE
#endif /* WABT_STACK_ALLOCATOR_STATS */
}

static int stack_setjmp_handler(WabtAllocator* allocator) {
  WabtStackAllocator* stack_allocator = (WabtStackAllocator*)allocator;
  stack_allocator->has_jmpbuf = WABT_TRUE;
  return setjmp(stack_allocator->jmpbuf);
}

WabtResult wabt_init_stack_allocator(WabtStackAllocator* stack_allocator,
                                     WabtAllocator* fallback) {
  WABT_ZERO_MEMORY(*stack_allocator);
  stack_allocator->allocator.alloc = stack_alloc;
  stack_allocator->allocator.realloc = stack_realloc;
  stack_allocator->allocator.free = stack_free;
  stack_allocator->allocator.destroy = stack_destroy;
  stack_allocator->allocator.mark = stack_mark;
  stack_allocator->allocator.reset_to_mark = stack_reset_to_mark;
  stack_allocator->allocator.print_stats = stack_print_stats;
  stack_allocator->allocator.setjmp_handler = stack_setjmp_handler;
  stack_allocator->fallback = fallback;

  WabtStackAllocatorChunk* chunk =
      allocate_chunk(stack_allocator, CHUNK_MAX_AVAIL, __FILE__, __LINE__);
  chunk->prev = NULL;
  stack_allocator->first = stack_allocator->last = chunk;
  return WABT_OK;
}

void wabt_destroy_stack_allocator(WabtStackAllocator* stack_allocator) {
  stack_destroy((WabtAllocator*)stack_allocator);
}
