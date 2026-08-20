/*****************************************************
 * RIFT POOL RUNTIME — automatic arena adapter
 *
 * The public raw allocation/refcount ABI remains here. Physical managed
 * blocks are owned by the private boundary-tag segregated heap; stack-like
 * bump allocations share the one uncommitted arena extent from above.
 *****************************************************/

#include "pools.h"

#include "arena.h"
#include "error_sink.h"
#include "segregated_heap.h"

#include <stdint.h>
#include <stdlib.h>

static char *bump_base;
static size_t bump_top;
static size_t arena_capacity;
static rift_arena_region pool_region;
static rift_oom_handler_fn oom_handler;

#ifndef RIFT_MEMORY_MAX_VALUE
#define RIFT_MEMORY_MAX_VALUE 0
#endif
#ifndef RIFT_MEMORY_MAX_PRESENT
#define RIFT_MEMORY_MAX_PRESENT 0
#endif
#ifndef RIFT_MEMORY_MIN_VALUE
#define RIFT_MEMORY_MIN_VALUE 0
#endif
#ifndef RIFT_MEMORY_MIN_PRESENT
#define RIFT_MEMORY_MIN_PRESENT 0
#endif
#ifndef RIFT_MEMORY_RESERVE_VALUE
#define RIFT_MEMORY_RESERVE_VALUE 0
#endif
#ifndef RIFT_MEMORY_RESERVE_PRESENT
#define RIFT_MEMORY_RESERVE_PRESENT 0
#endif

static const rift_arena_options rift_build_arena_options = {
    .memory_max = (size_t)RIFT_MEMORY_MAX_VALUE,
#ifdef __SDCC
    /* ZXN minimum headroom is a post-link driver acceptance check. */
    .memory_min = 0,
#else
    .memory_min = (size_t)RIFT_MEMORY_MIN_VALUE,
#endif
    .memory_reserve = (size_t)RIFT_MEMORY_RESERVE_VALUE,
    .memory_max_present = RIFT_MEMORY_MAX_PRESENT,
#ifdef __SDCC
    .memory_min_present = 0,
#else
    .memory_min_present = RIFT_MEMORY_MIN_PRESENT,
#endif
    .memory_reserve_present = RIFT_MEMORY_RESERVE_PRESENT,
};

#if !defined(__SDCC) || defined(RIFT_ALLOCATOR_STATS)
#define RIFT_POOL_STATS_ENABLED 1
static rift_allocator_stats allocator_stats;
#define RIFT_POOL_STAT_INC(field) (allocator_stats.field++)
#else
#define RIFT_POOL_STATS_ENABLED 0
#define RIFT_POOL_STAT_INC(field) ((void)0)
#endif

static size_t pool_alignment(void) {
#ifdef __SDCC
  return sizeof(void *);
#else
  return _Alignof(max_align_t);
#endif
}

static int round_up(size_t value, size_t alignment, size_t *result) {
  size_t remainder = value % alignment;
  if (remainder != 0) {
    size_t add = alignment - remainder;
    if (value > SIZE_MAX - add) return 0;
    value += add;
  }
  *result = value;
  return 1;
}

static void default_oom(const char *pool_name, size_t requested,
                        size_t available) {
  rift_error_text("rift_pools: ");
  rift_error_text(pool_name);
  rift_error_text(" pool exhausted; requested ");
  rift_error_size(requested);
  rift_error_text(" bytes, ");
  rift_error_size(available);
  rift_error_text(" available\n");
  exit(1);
}

void rift_set_oom_handler(rift_oom_handler_fn handler) {
  oom_handler = handler ? handler : default_oom;
}

void rift_pools_init(const rift_arena_options *requested_options) {
  rift_arena_options arena_options = requested_options
                                         ? *requested_options
                                         : rift_build_arena_options;
  if (!rift_arena_init(&arena_options) ||
      !rift_arena_acquire(0, &pool_region)) {
    rift_error_text("rift_pools: failed to acquire managed arena\n");
    exit(1);
  }
  arena_capacity =
      pool_region.capacity - pool_region.capacity % RIFT_ARENA_ALIGNMENT;
  if (arena_capacity < rift_heap_minimum_physical_block() ||
      arena_capacity >= (size_t)((rift_pool_offset_t)-1) ||
      !rift_heap_init(pool_region.base, arena_capacity,
#ifdef RIFT_ZXN_NO_BUMP_POOL
                      1
#else
                      0
#endif
                      )) {
    rift_error_text("rift_pools: invalid managed arena capacity\n");
    exit(1);
  }
#ifdef RIFT_ZXN_NO_BUMP_POOL
  bump_base = NULL;
#else
  bump_base = (char *)pool_region.base + arena_capacity;
#endif
  bump_top = 0;
  oom_handler = default_oom;
  rift_allocator_stats_reset();
}

#ifndef __SDCC
void rift_pools_deinit(void) {
  rift_heap_deinit();
  rift_arena_release(&pool_region);
  rift_arena_deinit();
  bump_base = NULL;
  bump_top = 0;
  arena_capacity = 0;
  pool_region = (rift_arena_region){0};
  oom_handler = default_oom;
}
#endif

void *rift_bump_alloc(size_t bytes) {
#ifdef RIFT_ZXN_NO_BUMP_POOL
  oom_handler("bump", bytes, 0);
  return NULL;
#else
  size_t aligned;
  size_t new_top;
  size_t limit;
  if (!round_up(bytes, pool_alignment(), &aligned) ||
      bump_top > SIZE_MAX - aligned) {
    oom_handler("bump", bytes, 0);
    return NULL;
  }
  new_top = bump_top + aligned;
  limit = new_top <= arena_capacity ? arena_capacity - new_top : 0;
  if (new_top > arena_capacity || !rift_heap_set_limit(limit)) {
    rift_collect();
    if (new_top > arena_capacity || !rift_heap_set_limit(limit)) {
      size_t committed = rift_heap_committed();
      size_t available = committed + bump_top <= arena_capacity
                             ? arena_capacity - committed - bump_top
                             : 0;
      oom_handler("bump", bytes, available);
      return NULL;
    }
  }
  bump_top = new_top;
  return bump_base - bump_top;
#endif
}

#if !defined(__SDCC) || !defined(RIFT_ZXN_NO_BUMP_POOL)
rift_bump_mark rift_bump_save(void) { return bump_top; }
void rift_bump_restore(rift_bump_mark mark) {
#ifndef RIFT_ZXN_NO_BUMP_POOL
  if (mark <= bump_top) {
    bump_top = mark;
    (void)rift_heap_set_limit(arena_capacity - bump_top);
  }
#else
  (void)mark;
#endif
}
#endif

void *rift_longlived_alloc(size_t payload_size) {
  void *payload = rift_heap_try_alloc(payload_size);
  if (!payload) {
    oom_handler("longlived", payload_size,
                rift_heap_largest_free_payload());
    return NULL;
  }
  RIFT_POOL_STAT_INC(allocations);
  return payload;
}

void rift_longlived_free(void *payload) {
  rift_heap_free_result result;
  rift_block_header *header;
  if (!payload) return;
  result = rift_heap_try_free(payload);
  if (result == RIFT_HEAP_FREE_OK) {
    RIFT_POOL_STAT_INC(frees);
    return;
  }
  if (result != RIFT_HEAP_FREE_NOT_OWNED) goto invalid_free;
  header = ((rift_block_header *)payload) - 1;
  if (header->refcount == RIFT_RC_STATIC) return;
invalid_free:
  rift_error_text("rift_pools: invalid free\n");
  exit(1);
}

void rift_collect(void) { RIFT_POOL_STAT_INC(collect_calls); }

size_t rift_bump_used(void) { return bump_top; }

size_t rift_bump_capacity(void) {
#ifdef RIFT_ZXN_NO_BUMP_POOL
  return 0;
#else
  size_t committed = rift_heap_committed();
  return committed <= arena_capacity ? arena_capacity - committed : 0;
#endif
}

size_t rift_longlived_used(void) { return rift_heap_live_bytes(); }
size_t rift_longlived_peak_used(void) { return rift_heap_peak_live_bytes(); }
size_t rift_longlived_capacity(void) { return arena_capacity - bump_top; }
size_t rift_longlived_free_bytes(void) {
  return rift_heap_free_physical_bytes();
}
size_t rift_longlived_largest_free_block(void) {
  return rift_heap_largest_free_payload();
}

void rift_allocator_stats_reset(void) {
#if RIFT_POOL_STATS_ENABLED
  allocator_stats = (rift_allocator_stats){0};
  rift_heap_stats_reset();
#endif
}

rift_allocator_stats rift_allocator_stats_get(void) {
  rift_allocator_stats result = {0};
#if RIFT_POOL_STATS_ENABLED
  rift_heap_stats heap = rift_heap_stats_get();
  result = allocator_stats;
  result.allocation_search_steps = heap.allocation_bin_visits;
  result.free_search_steps = heap.free_bin_visits;
  result.class_rounding_bytes = heap.class_rounding_bytes;
  result.coalesces = heap.coalesces;
  result.splits = heap.splits;
  result.capacity_failures = heap.capacity_failures;
  result.fragmentation_failures = heap.fragmentation_failures;
#endif
  return result;
}
