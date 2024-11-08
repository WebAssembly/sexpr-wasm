/*
 * Copyright 2018 WebAssembly Community Group participants
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

// This file is used as a template to generate code for regular memories or for
// shared memories. For this, the file must be included after defining either
// WASM_RT_MEM_OPS or WASM_RT_MEM_OPS_SHARED.

#if defined(WASM_RT_MEM_OPS) && defined(WASM_RT_MEM_OPS_SHARED)
#error \
    "Expected only one of { WASM_RT_MEM_OPS, WASM_RT_MEM_OPS_SHARED } to be defined"
#elif !defined(WASM_RT_MEM_OPS) && !defined(WASM_RT_MEM_OPS_SHARED)
#error \
    "Expected one of { WASM_RT_MEM_OPS, WASM_RT_MEM_OPS_SHARED } to be defined"
#endif

// Shared memory operations are defined only if we have C11
#if defined(WASM_RT_MEM_OPS) || \
    (defined(WASM_RT_MEM_OPS_SHARED) && defined(WASM_RT_C11_AVAILABLE))

#ifdef WASM_RT_MEM_OPS

// Memory operations on wasm_rt_memory_t
#define MEMORY_TYPE wasm_rt_memory_t
#define MEMORY_API_NAME(name) name
#define MEMORY_CELL_TYPE uint8_t*
#define MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name)

#else

// Memory operations on wasm_rt_shared_memory_t
#define MEMORY_TYPE wasm_rt_shared_memory_t
#define MEMORY_API_NAME(name) name##_shared
#define MEMORY_CELL_TYPE _Atomic volatile uint8_t*

#if WASM_RT_USE_C11THREADS
#define MEMORY_LOCK_VAR_INIT(name) C11_MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name) C11_MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name) C11_MEMORY_LOCK_RELEASE(name)
#elif WASM_RT_USE_PTHREADS
#define MEMORY_LOCK_VAR_INIT(name) PTHREAD_MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name) PTHREAD_MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name) PTHREAD_MEMORY_LOCK_RELEASE(name)
#elif WASM_RT_USE_CRITICALSECTION
#define MEMORY_LOCK_VAR_INIT(name) WIN_MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name) WIN_MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name) WIN_MEMORY_LOCK_RELEASE(name)
#endif

#endif

bool MEMORY_API_NAME(wasm_rt_memory_is_default32)(const MEMORY_TYPE* memory) {
  return memory->page_size == WASM_DEFAULT_PAGE_SIZE && !memory->is64;
}

void MEMORY_API_NAME(wasm_rt_allocate_memory)(MEMORY_TYPE* memory,
                                              uint64_t initial_pages,
                                              uint64_t max_pages,
                                              bool is64,
                                              uint32_t page_size) {
  uint64_t byte_length = initial_pages * page_size;
  memory->size = byte_length;
  memory->pages = initial_pages;
  memory->max_pages = max_pages;
  memory->is64 = is64;
  memory->page_size = page_size;
  MEMORY_LOCK_VAR_INIT(memory->mem_lock);

  if (WASM_RT_USE_MMAP &&
      MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
#if WASM_RT_USE_MMAP  // mmap-related functions don't exist unless this is set
    const uint64_t mmap_size =
        get_alloc_size_for_mmap_default32(memory->max_pages);
    void* addr = os_mmap(mmap_size);
    if (!addr) {
      os_print_last_error("os_mmap failed.");
      abort();
    }
    int ret = os_mprotect(addr, byte_length);
    if (ret != 0) {
      os_print_last_error("os_mprotect failed.");
      abort();
    }
    memory->data = addr;
#endif
  } else {
    memory->data = calloc(byte_length, 1);
  }
}

static uint64_t MEMORY_API_NAME(grow_memory_impl)(MEMORY_TYPE* memory,
                                                  uint64_t delta) {
  uint64_t old_pages = memory->pages;
  uint64_t new_pages = memory->pages + delta;
  if (new_pages == 0) {
    return 0;
  }
  if (new_pages < old_pages || new_pages > memory->max_pages) {
    return (uint64_t)-1;
  }
  uint64_t old_size = old_pages * memory->page_size;
  uint64_t new_size = new_pages * memory->page_size;
  uint64_t delta_size = delta * memory->page_size;
  MEMORY_CELL_TYPE new_data;
  if (WASM_RT_USE_MMAP &&
      MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
#if WASM_RT_USE_MMAP
    new_data = memory->data;
    int ret = os_mprotect((void*)(new_data + old_size), delta_size);
    if (ret != 0) {
      return (uint64_t)-1;
    }
#endif
  } else {
    new_data = realloc((void*)memory->data, new_size);
    if (new_data == NULL) {
      return (uint64_t)-1;
    }
#if !WABT_BIG_ENDIAN
    memset((void*)(new_data + old_size), 0, delta_size);
#endif
  }
#if WABT_BIG_ENDIAN
  memmove((void*)(new_data + new_size - old_size), (void*)new_data, old_size);
  memset((void*)new_data, 0, delta_size);
#endif
  memory->pages = new_pages;
  memory->size = new_size;
  memory->data = new_data;
  return old_pages;
}

uint64_t MEMORY_API_NAME(wasm_rt_grow_memory)(MEMORY_TYPE* memory,
                                              uint64_t delta) {
  MEMORY_LOCK_AQUIRE(memory->mem_lock);
  uint64_t ret = MEMORY_API_NAME(grow_memory_impl)(memory, delta);
  MEMORY_LOCK_RELEASE(memory->mem_lock);
#ifdef WASM_RT_GROW_FAILED_HANDLER
  if (ret == (uint64_t)-1) {
    WASM_RT_GROW_FAILED_HANDLER();
  }
#endif
  return ret;
}

void MEMORY_API_NAME(wasm_rt_free_memory)(MEMORY_TYPE* memory) {
  if (WASM_RT_USE_MMAP &&
      MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
#if WASM_RT_USE_MMAP
    const uint64_t mmap_size =
        get_alloc_size_for_mmap_default32(memory->max_pages);
    os_munmap((void*)memory->data, mmap_size);  // ignore error
#endif
  } else {
    free((void*)memory->data);
  }
}

#undef MEMORY_LOCK_RELEASE
#undef MEMORY_LOCK_AQUIRE
#undef MEMORY_LOCK_VAR_INIT
#undef MEMORY_CELL_TYPE
#undef MEMORY_API_NAME
#undef MEMORY_TYPE

#endif
