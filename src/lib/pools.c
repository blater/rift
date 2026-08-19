/*****************************************************
 * RIFT POOL RUNTIME — bounded long-lived allocator
 *
 * The long-lived pool uses small LIFO magazines backed by intrusive buddy
 * lists. No normal path searches a list of heap blocks: selecting a class,
 * splitting, and coalescing are bounded by the number of buddy orders.
 *****************************************************/

#include "pools.h"
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
#define RIFT_BUDDY_MAX_ORDERS 9
#define RIFT_BUDDY_MAX_ROOTS 2
/* Z88DK does not guarantee that a global union is placed at its member's
 * alignment. Reserve one byte and align the usable base ourselves. */
#ifndef RIFT_ZXN_NO_BUMP_POOL
static char zxn_bump_pool_storage[RIFT_ZXN_BUMP_POOL_CAPACITY + 1];
#endif
static char zxn_longlived_pool_storage[RIFT_ZXN_LONGLIVED_POOL_CAPACITY + 1];
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
static size_t bump_cap = 0;
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
static size_t ll_core_free_bytes = 0;
static rift_pool_offset_t buddy_heads[RIFT_BUDDY_MAX_ORDERS];
static rift_buddy_root buddy_roots[RIFT_BUDDY_MAX_ROOTS];
static unsigned int buddy_root_count = 0;
static const unsigned int magazine_limits[RIFT_MAGAZINE_COUNT] = {4, 2, 1};
static rift_oom_handler_fn oom_handler = NULL;
static rift_allocator_stats allocator_stats;

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

#ifdef __SDCC
static char *align_pool_base(char *base, size_t alignment) {
  uintptr_t value = (uintptr_t)base;
  value = (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
  return (char *)value;
}
#endif

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
  unsigned int i;
  for (i = 0; i < buddy_root_count; i++) {
    size_t root_size = bytes_for_order(buddy_roots[i].max_order);
    if (offset >= buddy_roots[i].offset &&
        (size_t)(offset - buddy_roots[i].offset) < root_size) return &buddy_roots[i];
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
  for (order = wanted_order; order < RIFT_BUDDY_MAX_ORDERS; order++) {
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

void rift_pools_init(size_t bump_capacity, size_t longlived_capacity) {
  size_t alignment = pool_alignment();
  size_t minimum_quantum = RIFT_HEADER_SIZE + sizeof(rift_pool_offset_t);
  size_t units;
  size_t root_offset = 0;
#ifdef __SDCC
  if (longlived_capacity > RIFT_ZXN_LONGLIVED_POOL_CAPACITY) {
    rift_error_text("rift_pools: requested ZXN pool capacity exceeds static budget\n");
    exit(1);
  }
#ifndef RIFT_ZXN_NO_BUMP_POOL
  if (bump_capacity > RIFT_ZXN_BUMP_POOL_CAPACITY) {
    rift_error_text("rift_pools: requested ZXN bump capacity exceeds static budget\n");
    exit(1);
  }
  bump_base = align_pool_base(zxn_bump_pool_storage, alignment);
#else
  (void)bump_capacity;
  bump_base = NULL;
#endif
  ll_base = align_pool_base(zxn_longlived_pool_storage, alignment);
  if (
#ifndef RIFT_ZXN_NO_BUMP_POOL
      ((uintptr_t)bump_base % alignment) != 0 ||
#endif
      ((uintptr_t)ll_base % alignment) != 0) {
    rift_error_text("rift_pools: target pool alignment failure\n");
    exit(1);
  }
#else
  bump_base = (char *)malloc(bump_capacity);
  ll_base = (char *)malloc(longlived_capacity);
  if (!bump_base || !ll_base) {
    rift_error_text("rift_pools: failed to allocate host pool backing\n");
    free(bump_base);
    free(ll_base);
    bump_base = NULL;
    ll_base = NULL;
    exit(1);
  }
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
#ifdef RIFT_ZXN_NO_BUMP_POOL
  bump_cap = 0;
#else
  bump_cap = bump_capacity - (bump_capacity % alignment);
#endif
  ll_cap = longlived_capacity - (longlived_capacity % ll_quantum);
  if (!offset_fits(ll_cap) || ll_cap < ll_quantum) {
    rift_error_text("rift_pools: invalid longlived pool capacity\n");
    exit(1);
  }
  bump_top = 0;
  ll_live_bytes = 0;
  ll_peak_live_bytes = 0;
  ll_core_free_bytes = 0;
  ll_magazine_free_bytes = 0;
  reset_heads();
  units = ll_cap / ll_quantum;
  while (units != 0) {
    size_t root_units = 1;
    unsigned int order = 0;
    rift_block_header *root;
    while (root_units <= units / 2) {
      root_units <<= 1;
      order++;
    }
    if (order >= RIFT_BUDDY_MAX_ORDERS || buddy_root_count >= RIFT_BUDDY_MAX_ROOTS) {
      rift_error_text("rift_pools: buddy order exceeds target representation\n");
      exit(1);
    }
    root = (rift_block_header *)(ll_base + root_offset);
    buddy_roots[buddy_root_count].offset = (rift_pool_offset_t)root_offset;
    buddy_roots[buddy_root_count].max_order = order;
    buddy_root_count++;
    core_insert(root, order);
    root_offset += root_units * ll_quantum;
    units -= root_units;
  }
  rift_allocator_stats_reset();
  if (!oom_handler) oom_handler = default_oom;
}

void rift_pools_deinit(void) {
#ifndef __SDCC
  free(bump_base);
  free(ll_base);
#endif
  bump_base = NULL;
  bump_top = 0;
  bump_cap = 0;
  ll_base = NULL;
  ll_cap = 0;
  ll_quantum = 0;
  ll_live_bytes = 0;
  ll_peak_live_bytes = 0;
  ll_core_free_bytes = 0;
  ll_magazine_free_bytes = 0;
  reset_heads();
}

void *rift_bump_alloc(size_t bytes) {
  size_t aligned = 0;
  if (!round_up(bytes, pool_alignment(), &aligned) || aligned > bump_cap - bump_top) {
    oom_handler("bump", bytes, bump_cap - bump_top);
    return NULL;
  }
  bytes = bump_top;
  bump_top += aligned;
  return bump_base + bytes;
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
  if (payload_size > SIZE_MAX - RIFT_HEADER_SIZE ||
      payload_size > ll_cap - RIFT_HEADER_SIZE) {
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
}

void rift_longlived_reclaim(void) { rift_collect(); }

size_t rift_bump_used(void) { return bump_top; }
size_t rift_bump_capacity(void) { return bump_cap; }
size_t rift_longlived_used(void) { return ll_live_bytes; }
size_t rift_longlived_peak_used(void) { return ll_peak_live_bytes; }
size_t rift_longlived_capacity(void) { return ll_cap; }
size_t rift_longlived_free_bytes(void) { return ll_core_free_bytes + ll_magazine_free_bytes; }

size_t rift_longlived_largest_free_block(void) {
  int order;
  for (order = RIFT_BUDDY_MAX_ORDERS - 1; order >= 0; order--) {
    if (buddy_heads[order] != RIFT_POOL_OFFSET_NONE) return bytes_for_order((unsigned int)order) - RIFT_HEADER_SIZE;
  }
  for (order = RIFT_MAGAZINE_COUNT - 1; order >= 0; order--) {
    if (magazine_heads[order] != RIFT_POOL_OFFSET_NONE) return bytes_for_order((unsigned int)order) - RIFT_HEADER_SIZE;
  }
  return 0;
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
