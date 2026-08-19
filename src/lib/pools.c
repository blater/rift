/*****************************************************
 * RIFT POOL RUNTIME — elastic bounded-work allocator
 *
 * The long-lived pool uses small LIFO magazines backed by intrusive buddy
 * lists. No normal path searches a list of heap blocks: selecting a class,
 * splitting, and coalescing are bounded by the number of buddy orders. On
 * ZX Next both allocators share the linker gap below the protected stack.
 *****************************************************/

#include "pools.h"
#include "arena.h"
#include "error_sink.h"
#include <limits.h>
#include <stdlib.h>

/* The normal ZXN build exports the public allocate/free symbols from the
 * small assembly shim.  It handles only a magazine hit and transfers every
 * other request to these C implementations.  Keeping the buddy core here
 * gives misses, full magazines, diagnostics, and collection one source of
 * truth.  Instrumented builds deliberately use C throughout so their
 * structural counters remain exact. */
#if defined(__SDCC) && defined(RIFT_POOL_ASM_FAST)
#define rift_longlived_alloc rift_longlived_alloc_slow
#define rift_longlived_free rift_longlived_free_slow
#endif

#define RIFT_HEADER_SIZE (sizeof(rift_block_header))
#define RIFT_POOL_OFFSET_NONE ((rift_pool_offset_t)-1)
#define RIFT_MAGAZINE_COUNT 3

#ifdef __SDCC
#define RIFT_BUDDY_MAX_ORDERS 12
#define RIFT_BUDDY_MAX_ROOTS 32
#else
#define RIFT_BUDDY_MAX_ORDERS (sizeof(size_t) * CHAR_BIT)
#define RIFT_BUDDY_MAX_ROOTS (sizeof(size_t) * CHAR_BIT)
#endif

typedef struct rift_buddy_root {
  rift_pool_offset_t offset;
  unsigned int max_order;
} rift_buddy_root;

static char *bump_base = NULL;
static size_t bump_top = 0;
static size_t arena_capacity = 0;
static rift_arena_region pool_region;
#ifdef __SDCC
char *rift_ll_base = NULL;
size_t rift_ll_live_bytes = 0;
size_t rift_ll_peak_live_bytes = 0;
size_t rift_ll_magazine_free_bytes = 0;
size_t rift_ll_cap = 0;
rift_pool_offset_t rift_magazine_heads[RIFT_MAGAZINE_COUNT];
unsigned int rift_magazine_counts[RIFT_MAGAZINE_COUNT];
#define ll_base rift_ll_base
#define ll_live_bytes rift_ll_live_bytes
#define ll_peak_live_bytes rift_ll_peak_live_bytes
#define ll_magazine_free_bytes rift_ll_magazine_free_bytes
#define ll_cap rift_ll_cap
#define magazine_heads rift_magazine_heads
#define magazine_counts rift_magazine_counts
#else
static char *ll_base = NULL;
static size_t ll_live_bytes = 0;
static size_t ll_peak_live_bytes = 0;
static size_t ll_magazine_free_bytes = 0;
static rift_pool_offset_t magazine_heads[RIFT_MAGAZINE_COUNT];
static unsigned int magazine_counts[RIFT_MAGAZINE_COUNT];
#endif
#ifndef __SDCC
static size_t ll_cap = 0;
#endif
static size_t ll_quantum = 0;
static unsigned int buddy_order_count = 0;
static size_t ll_core_free_bytes = 0;
static rift_pool_offset_t buddy_heads[RIFT_BUDDY_MAX_ORDERS];
static rift_buddy_root buddy_roots[RIFT_BUDDY_MAX_ROOTS];
static unsigned int buddy_root_count = 0;
static const unsigned int magazine_limits[RIFT_MAGAZINE_COUNT] = {4, 2, 1};
static rift_oom_handler_fn oom_handler = NULL;
static rift_allocator_stats allocator_stats;

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

/* Per-operation counters are valuable in host tests and deliberately-enabled
 * target diagnostics, but are not part of the Z80 allocator fast path. */
#if !defined(__SDCC) || defined(RIFT_ALLOCATOR_STATS)
#define RIFT_STAT_INC(field) (allocator_stats.field++)
#else
#define RIFT_STAT_INC(field) ((void)0)
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

static int offset_fits(size_t offset) {
  return offset < (size_t)RIFT_POOL_OFFSET_NONE;
}

static rift_block_header *block_at(rift_pool_offset_t offset) {
  if (offset == RIFT_POOL_OFFSET_NONE) return NULL;
  return (rift_block_header *)(ll_base + offset);
}

static rift_pool_offset_t block_offset(const rift_block_header *block) {
  return (rift_pool_offset_t)((const char *)block - ll_base);
}

static rift_pool_offset_t *block_previous(rift_block_header *block) {
  return (rift_pool_offset_t *)((char *)block + RIFT_HEADER_SIZE);
}

static size_t block_total_size(const rift_block_header *block) {
  return (size_t)block->size + RIFT_HEADER_SIZE;
}

static unsigned int order_for_total(size_t total) {
  unsigned int order = 0;
  size_t size = ll_quantum;
  while (size < total) {
    size <<= 1;
    order++;
  }
  return order;
}

static size_t bytes_for_order(unsigned int order) {
  return ll_quantum << order;
}

static void set_block_size(rift_block_header *block, unsigned int order) {
  block->size = (rift_pool_offset_t)(bytes_for_order(order) - RIFT_HEADER_SIZE);
}

static int magazine_class(unsigned int order) {
  return order < RIFT_MAGAZINE_COUNT ? (int)order : -1;
}

static void reset_heads(void) {
  unsigned int i;
  for (i = 0; i < RIFT_BUDDY_MAX_ORDERS; i++) buddy_heads[i] = RIFT_POOL_OFFSET_NONE;
  for (i = 0; i < RIFT_MAGAZINE_COUNT; i++) {
    magazine_heads[i] = RIFT_POOL_OFFSET_NONE;
    magazine_counts[i] = 0;
  }
  buddy_root_count = 0;
}

static void core_insert(rift_block_header *block, unsigned int order) {
  rift_pool_offset_t offset = block_offset(block);
  rift_pool_offset_t old_head = buddy_heads[order];
  block->refcount = RIFT_RC_FREE;
  set_block_size(block, order);
  block->next_free = old_head;
  *block_previous(block) = RIFT_POOL_OFFSET_NONE;
  if (old_head != RIFT_POOL_OFFSET_NONE) *block_previous(block_at(old_head)) = offset;
  buddy_heads[order] = offset;
  ll_core_free_bytes += block->size;
}

static void core_remove(rift_block_header *block, unsigned int order) {
  rift_pool_offset_t previous = *block_previous(block);
  rift_pool_offset_t next = block->next_free;
  if (previous == RIFT_POOL_OFFSET_NONE) buddy_heads[order] = next;
  else block_at(previous)->next_free = next;
  if (next != RIFT_POOL_OFFSET_NONE) *block_previous(block_at(next)) = previous;
  ll_core_free_bytes -= block->size;
  block->next_free = RIFT_POOL_OFFSET_NONE;
}

static const rift_buddy_root *root_for_block(const rift_block_header *block) {
  rift_pool_offset_t offset = block_offset(block);
  unsigned int low = 0;
  unsigned int high = buddy_root_count;
  while (low < high) {
    unsigned int middle = low + (high - low) / 2;
    const rift_buddy_root *root = &buddy_roots[middle];
    size_t root_size = bytes_for_order(root->max_order);
    if (offset < root->offset)
      high = middle;
    else if ((size_t)(offset - root->offset) >= root_size)
      low = middle + 1;
    else
      return root;
  }
  return NULL;
}

static void core_free(rift_block_header *block) {
  unsigned int order = order_for_total(block_total_size(block));
  const rift_buddy_root *root = root_for_block(block);
  if (!root) {
    rift_error_text("rift_pools: free outside buddy roots\n");
    exit(1);
  }
  while (order < root->max_order) {
    size_t block_size = bytes_for_order(order);
    rift_pool_offset_t offset = block_offset(block);
    rift_pool_offset_t buddy_offset =
        (rift_pool_offset_t)(root->offset + ((size_t)(offset - root->offset) ^ block_size));
    rift_block_header *buddy = block_at(buddy_offset);
    if (buddy->refcount != RIFT_RC_FREE ||
        order_for_total(block_total_size(buddy)) != order) break;
    core_remove(buddy, order);
    if (buddy_offset < offset) block = buddy;
    order++;
    RIFT_STAT_INC(coalesces);
  }
  core_insert(block, order);
}

static rift_block_header *core_take(unsigned int wanted_order) {
  unsigned int order;
  for (order = wanted_order; order < buddy_order_count; order++) {
    rift_pool_offset_t head = buddy_heads[order];
    rift_block_header *block;
    if (head == RIFT_POOL_OFFSET_NONE) continue;
    block = block_at(head);
    core_remove(block, order);
    while (order > wanted_order) {
      rift_block_header *upper;
      size_t half;
      order--;
      half = bytes_for_order(order);
      upper = (rift_block_header *)((char *)block + half);
      core_insert(upper, order);
      set_block_size(block, order);
      RIFT_STAT_INC(buddy_splits);
    }
    return block;
  }
  return NULL;
}

static size_t arena_uncommitted_bytes(void) {
  size_t occupied = ll_cap + bump_top;
  return occupied <= arena_capacity ? arena_capacity - occupied : 0;
}

static size_t arena_longlived_max_capacity(void) {
  return arena_capacity - bump_top;
}

static size_t arena_bump_effective_capacity(void) {
#ifdef RIFT_ZXN_NO_BUMP_POOL
  return 0;
#else
  return arena_capacity - ll_cap;
#endif
}

static int arena_grow_longlived(unsigned int wanted_order) {
  size_t available;
  size_t root_size;
  size_t maximum = arena_longlived_max_capacity();
  unsigned int order = wanted_order;
  rift_block_header *root;
  if (wanted_order >= buddy_order_count || ll_cap >= maximum ||
      buddy_root_count >= RIFT_BUDDY_MAX_ROOTS)
    return 0;
  available = maximum - ll_cap;
  if (available > arena_uncommitted_bytes()) available = arena_uncommitted_bytes();
  if (bytes_for_order(wanted_order) > available) return 0;

  /* Geometric roots keep metadata bounded without claiming the whole arena
   * for the first small allocation. Fall back to the largest root that fits
   * near a cap or the opposing bump frontier. */
  if (buddy_root_count != 0) {
    unsigned int next_order =
        buddy_roots[buddy_root_count - 1].max_order + 1;
    if (next_order > order) order = next_order;
  }
  if (order >= buddy_order_count) order = buddy_order_count - 1;
  while (order > wanted_order && bytes_for_order(order) > available) order--;
  root_size = bytes_for_order(order);
  root = (rift_block_header *)(ll_base + ll_cap);
  buddy_roots[buddy_root_count].offset = (rift_pool_offset_t)ll_cap;
  buddy_roots[buddy_root_count].max_order = order;
  buddy_root_count++;
  ll_cap += root_size;
  core_insert(root, order);
  return 1;
}

static void arena_release_free_tail_roots(void) {
  while (buddy_root_count != 0) {
    rift_buddy_root *root = &buddy_roots[buddy_root_count - 1];
    rift_block_header *block = block_at(root->offset);
    if (block->refcount != RIFT_RC_FREE ||
        block_total_size(block) != bytes_for_order(root->max_order))
      break;
    core_remove(block, root->max_order);
    ll_cap = root->offset;
    buddy_root_count--;
  }
}

static rift_block_header *magazine_take(int class_index) {
  rift_pool_offset_t offset = magazine_heads[class_index];
  rift_block_header *block;
  if (offset == RIFT_POOL_OFFSET_NONE) return NULL;
  block = block_at(offset);
  magazine_heads[class_index] = block->next_free;
  magazine_counts[class_index]--;
  ll_magazine_free_bytes -= block->size;
  block->next_free = RIFT_POOL_OFFSET_NONE;
  RIFT_STAT_INC(magazine_hits);
  return block;
}

static int magazine_store(rift_block_header *block, int class_index) {
  rift_pool_offset_t offset;
  if (magazine_counts[class_index] >= magazine_limits[class_index]) return 0;
  offset = block_offset(block);
  block->refcount = RIFT_RC_MAGAZINE;
  block->next_free = magazine_heads[class_index];
  magazine_heads[class_index] = offset;
  magazine_counts[class_index]++;
  ll_magazine_free_bytes += block->size;
  return 1;
}

static void default_oom(const char *pool_name, size_t requested, size_t available) {
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
  size_t alignment = pool_alignment();
  size_t minimum_quantum = RIFT_HEADER_SIZE + sizeof(rift_pool_offset_t);
  if (!rift_arena_init(&arena_options) ||
      !rift_arena_acquire(0, &pool_region)) {
    rift_error_text("rift_pools: failed to acquire managed arena\n");
    exit(1);
  }
  ll_base = (char *)pool_region.base;
  arena_capacity = pool_region.capacity;
  arena_capacity -= arena_capacity % RIFT_ARENA_ALIGNMENT;
  if (!offset_fits(arena_capacity)) {
    rift_error_text("rift_pools: managed arena exceeds allocator offsets\n");
    exit(1);
  }
#ifdef RIFT_ZXN_NO_BUMP_POOL
  bump_base = NULL;
#else
  bump_base = ll_base + arena_capacity;
#endif
#ifdef __SDCC
  (void)minimum_quantum;
  ll_quantum = 16;
#else
  if (!round_up(minimum_quantum > 16 ? minimum_quantum : 16, alignment, &ll_quantum)) {
    rift_error_text("rift_pools: pool capacity overflow\n");
    exit(1);
  }
#endif
  ll_cap = 0;
  buddy_order_count = 1;
  {
    size_t order_bytes = ll_quantum;
    while (buddy_order_count < RIFT_BUDDY_MAX_ORDERS &&
           order_bytes <= arena_capacity / 2) {
      order_bytes <<= 1;
      buddy_order_count++;
    }
  }
  if (arena_capacity < ll_quantum) {
    rift_error_text("rift_pools: invalid longlived pool capacity\n");
    exit(1);
  }
  bump_top = 0;
  ll_live_bytes = 0;
  ll_peak_live_bytes = 0;
  ll_core_free_bytes = 0;
  ll_magazine_free_bytes = 0;
  reset_heads();
  rift_allocator_stats_reset();
  oom_handler = default_oom;
}

#ifndef __SDCC
void rift_pools_deinit(void) {
  rift_arena_release(&pool_region);
  rift_arena_deinit();
  bump_base = NULL;
  bump_top = 0;
  arena_capacity = 0;
  ll_base = NULL;
  ll_cap = 0;
  ll_quantum = 0;
  buddy_order_count = 0;
  ll_live_bytes = 0;
  ll_peak_live_bytes = 0;
  ll_core_free_bytes = 0;
  ll_magazine_free_bytes = 0;
  reset_heads();
}
#endif

void *rift_bump_alloc(size_t bytes) {
  size_t aligned = 0;
  size_t capacity;
  if (!round_up(bytes, pool_alignment(), &aligned)) {
    oom_handler("bump", bytes, 0);
    return NULL;
  }
  capacity = arena_bump_effective_capacity();
  if (bump_top > capacity || aligned > capacity - bump_top) {
    rift_collect();
    capacity = arena_bump_effective_capacity();
  }
  if (bump_top > capacity || aligned > capacity - bump_top) {
    oom_handler("bump", bytes, bump_top <= capacity ? capacity - bump_top : 0);
    return NULL;
  }
  bump_top += aligned;
  return bump_base - bump_top;
}

rift_bump_mark rift_bump_save(void) { return bump_top; }

void rift_bump_restore(rift_bump_mark mark) {
  if (mark <= bump_top) bump_top = mark;
}

void *rift_longlived_alloc(size_t payload_size) {
  size_t total;
  unsigned int order;
  int class_index;
  rift_block_header *block;
  if (payload_size == 0) payload_size = 1;
  if (payload_size > SIZE_MAX - RIFT_HEADER_SIZE
      || arena_longlived_max_capacity() < RIFT_HEADER_SIZE
      || payload_size > arena_longlived_max_capacity() - RIFT_HEADER_SIZE
  ) {
    oom_handler("longlived", payload_size, rift_longlived_largest_free_block());
    return NULL;
  }
  total = payload_size + RIFT_HEADER_SIZE;
#ifdef __SDCC
  /* This is the steady-state target path for short string backing and small
   * handles. Keep it in this public entry point so generated programs do not
   * need a second allocator API. It deliberately avoids helper calls, order
   * walks, and list inspection on a magazine hit. */
  if (payload_size <= 58) {
    int fast_class;
    rift_pool_offset_t offset;
    if (payload_size <= 10) fast_class = 0;
    else if (payload_size <= 26) fast_class = 1;
    else fast_class = 2;
    offset = magazine_heads[fast_class];
    if (offset != RIFT_POOL_OFFSET_NONE) {
      rift_block_header *fast_block = (rift_block_header *)(ll_base + offset);
      magazine_heads[fast_class] = fast_block->next_free;
      magazine_counts[fast_class]--;
      ll_magazine_free_bytes -= fast_block->size;
      fast_block->next_free = RIFT_POOL_OFFSET_NONE;
      fast_block->refcount = 1;
      ll_live_bytes += (size_t)fast_block->size + RIFT_HEADER_SIZE;
      if (ll_live_bytes > ll_peak_live_bytes) ll_peak_live_bytes = ll_live_bytes;
      RIFT_STAT_INC(allocations);
      RIFT_STAT_INC(magazine_hits);
      return (char *)fast_block + RIFT_HEADER_SIZE;
    }
  }
#endif
  order = order_for_total(total);
  if (order >= RIFT_BUDDY_MAX_ORDERS) {
    oom_handler("longlived", payload_size, rift_longlived_largest_free_block());
    return NULL;
  }
  RIFT_STAT_INC(allocations);
  class_index = magazine_class(order);
  block = class_index >= 0 ? magazine_take(class_index) : NULL;
  if (!block) block = core_take(order);
  if (!block && arena_grow_longlived(order)) block = core_take(order);
  if (!block && ll_magazine_free_bytes != 0) {
    rift_collect();
    block = core_take(order);
    if (!block && arena_grow_longlived(order)) block = core_take(order);
  }
  if (!block) {
    oom_handler("longlived", payload_size, rift_longlived_largest_free_block());
    return NULL;
  }
  block->refcount = 1;
  block->next_free = RIFT_POOL_OFFSET_NONE;
  ll_live_bytes += block_total_size(block);
  if (ll_live_bytes > ll_peak_live_bytes) ll_peak_live_bytes = ll_live_bytes;
  return (char *)block + RIFT_HEADER_SIZE;
}

void rift_longlived_free(void *payload) {
  rift_block_header *block;
  unsigned int order;
  int class_index;
  if (!payload) return;
  block = ((rift_block_header *)payload) - 1;
  if (block->refcount == RIFT_RC_STATIC) return;
  if ((char *)block < ll_base || (char *)block >= ll_base + ll_cap) {
    rift_error_text("rift_pools: invalid free\n");
    exit(1);
  }
  if (block->refcount == RIFT_RC_FREE || block->refcount == RIFT_RC_MAGAZINE) {
    rift_error_text("rift_pools: double free\n");
    exit(1);
  }
  RIFT_STAT_INC(frees);
  ll_live_bytes -= block_total_size(block);
#ifdef __SDCC
  /* Mirror the allocation fast path. A block can only have one of these
   * capacities when it belongs to the corresponding 16/32/64-byte class. */
  if (block->size == 10 || block->size == 26 || block->size == 58) {
    int fast_class = block->size == 10 ? 0 : (block->size == 26 ? 1 : 2);
    if (magazine_counts[fast_class] < magazine_limits[fast_class]) {
      block->refcount = RIFT_RC_MAGAZINE;
      block->next_free = magazine_heads[fast_class];
      magazine_heads[fast_class] = block_offset(block);
      magazine_counts[fast_class]++;
      ll_magazine_free_bytes += block->size;
      return;
    }
  }
#endif
  order = order_for_total(block_total_size(block));
  class_index = magazine_class(order);
  if (class_index >= 0 && magazine_store(block, class_index)) return;
  core_free(block);
  arena_release_free_tail_roots();
}

void rift_collect(void) {
  unsigned int i;
  RIFT_STAT_INC(collect_calls);
  for (i = 0; i < RIFT_MAGAZINE_COUNT; i++) {
    while (magazine_heads[i] != RIFT_POOL_OFFSET_NONE) {
      rift_block_header *block = block_at(magazine_heads[i]);
      magazine_heads[i] = block->next_free;
      magazine_counts[i]--;
      ll_magazine_free_bytes -= block->size;
      core_free(block);
    }
  }
  arena_release_free_tail_roots();
}

size_t rift_bump_used(void) { return bump_top; }
size_t rift_bump_capacity(void) {
  return arena_bump_effective_capacity();
}
size_t rift_longlived_used(void) { return ll_live_bytes; }
size_t rift_longlived_peak_used(void) { return ll_peak_live_bytes; }
size_t rift_longlived_capacity(void) {
  return arena_longlived_max_capacity();
}
size_t rift_longlived_free_bytes(void) {
  size_t available = ll_core_free_bytes + ll_magazine_free_bytes;
  available += arena_uncommitted_bytes();
  return available;
}

size_t rift_longlived_largest_free_block(void) {
  int order;
  size_t largest = 0;
  for (order = (int)buddy_order_count - 1; order >= 0; order--) {
    if (buddy_heads[order] != RIFT_POOL_OFFSET_NONE) {
      largest = bytes_for_order((unsigned int)order) - RIFT_HEADER_SIZE;
      break;
    }
  }
  {
    size_t maximum = arena_longlived_max_capacity();
    size_t available = ll_cap < maximum ? maximum - ll_cap : 0;
    if (available > arena_uncommitted_bytes()) available = arena_uncommitted_bytes();
    for (order = (int)buddy_order_count - 1; order >= 0; order--) {
      size_t bytes = bytes_for_order((unsigned int)order);
      if (bytes <= available) {
        size_t payload = bytes - RIFT_HEADER_SIZE;
        if (payload > largest) largest = payload;
        break;
      }
    }
  }
  for (order = RIFT_MAGAZINE_COUNT - 1; order >= 0; order--) {
    if (magazine_heads[order] != RIFT_POOL_OFFSET_NONE) {
      size_t payload = bytes_for_order((unsigned int)order) - RIFT_HEADER_SIZE;
      if (payload > largest) largest = payload;
      break;
    }
  }
  return largest;
}

void rift_allocator_stats_reset(void) {
  allocator_stats.allocations = 0;
  allocator_stats.frees = 0;
  allocator_stats.allocation_scan_steps = 0;
  allocator_stats.free_scan_steps = 0;
  allocator_stats.coalesces = 0;
  allocator_stats.magazine_hits = 0;
  allocator_stats.buddy_splits = 0;
  allocator_stats.collect_calls = 0;
}

rift_allocator_stats rift_allocator_stats_get(void) { return allocator_stats; }
