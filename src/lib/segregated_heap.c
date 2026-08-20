#include "segregated_heap.h"

#include "pools.h"
#ifdef RIFT_HEAP_ROUTINE_COMPACTION
#include "segregated_heap_internal.h"
#endif

#include <limits.h>
#include <stdint.h>
#if defined(RIFT_ALLOCATOR_TEST) || defined(RIFT_HEAP_ROUTINE_COMPACTION)
#include <string.h>
#endif

#define RIFT_HEAP_MIN_FL 3u
#define RIFT_HEAP_SL_COUNT 8u
#define RIFT_HEAP_COLD 0xA5u
#define RIFT_HEAP_READY 0x5Au
#define RIFT_HEAP_LINK_NONE ((rift_pool_offset_t)-1)
#define RIFT_HEAP_SMALLEST_MAPPABLE 12u

#ifdef __SDCC
#define RIFT_HEAP_FL_COUNT 13u
typedef uint16_t rift_heap_fl_bitmap;
#else
#define RIFT_HEAP_FL_COUNT \
  ((unsigned int)(sizeof(size_t) * CHAR_BIT - RIFT_HEAP_MIN_FL))
typedef uint64_t rift_heap_fl_bitmap;
#endif

#define RIFT_HEAP_BIN_COUNT (RIFT_HEAP_FL_COUNT * RIFT_HEAP_SL_COUNT)

typedef uintptr_t rift_heap_link;

typedef struct rift_physical_block {
  rift_pool_offset_t previous_physical_size;
  rift_block_header public_header;
} rift_physical_block;

typedef struct rift_heap_class {
  unsigned char first;
  unsigned char second;
  size_t charged_physical;
} rift_heap_class;

static unsigned char *heap_base;
static size_t heap_capacity;
static size_t heap_limit;
static unsigned char heap_limit_permanent;
static size_t heap_committed_bytes;
static size_t heap_live;
static size_t heap_peak_live;
static size_t heap_free_physical;
static rift_physical_block *heap_tail;
static rift_heap_link heap_roots[RIFT_HEAP_BIN_COUNT];
static rift_heap_fl_bitmap heap_fl_bitmap;
static unsigned char heap_sl_bitmap[RIFT_HEAP_FL_COUNT];
#ifdef __SDCC
static unsigned char heap_selected_first;
static unsigned char heap_selected_second;
#endif
#if !defined(__SDCC) || defined(RIFT_ALLOCATOR_STATS)
#define RIFT_HEAP_STATS_ENABLED 1
static rift_heap_stats heap_stats;
#define RIFT_HEAP_STAT_INC(field) (heap_stats.field++)
#define RIFT_HEAP_STAT_ADD(field, amount) (heap_stats.field += (amount))
#else
#define RIFT_HEAP_STATS_ENABLED 0
#define RIFT_HEAP_STAT_INC(field) ((void)0)
#define RIFT_HEAP_STAT_ADD(field, amount) ((void)(amount))
#endif
static unsigned char heap_lifecycle = RIFT_HEAP_COLD;

#ifdef RIFT_HEAP_ROUTINE_COMPACTION
#define RIFT_HEAP_CURSOR_NONE ((size_t)-1)
typedef struct rift_heap_compact_cursor {
  size_t scan;
  size_t hole;
  uint16_t epoch;
} rift_heap_compact_cursor;

static rift_heap_compact_cursor heap_compact_cursor;
static uint16_t heap_movement_epoch;
unsigned char rift_managed_maintenance_due;

#ifdef RIFT_HEAP_COMPACTION_STATS
#define COMPACT_REPORT_INC(report, field) ((report)->field++)
#define COMPACT_REPORT_ADD(report, field, amount) ((report)->field += (amount))
#else
#define COMPACT_REPORT_INC(report, field) ((void)0)
#define COMPACT_REPORT_ADD(report, field, amount) ((void)0)
#endif

#ifdef __SDCC
typedef char rift_heap_cursor_size_assert[
    (sizeof(rift_heap_compact_cursor) == 6u) ? 1 : -1];
#endif

static void advance_movement_epoch(void) {
  if (heap_movement_epoch == UINT16_MAX) {
    heap_compact_cursor.scan = RIFT_HEAP_CURSOR_NONE;
    heap_movement_epoch = 0;
  } else {
    heap_movement_epoch++;
  }
}

static void note_layout_mutation(int raises_debt) {
  advance_movement_epoch();
  if (raises_debt) {
    if (rift_managed_maintenance_due != 0xffu)
      rift_managed_maintenance_due++;
  }
}
#else
#define note_layout_mutation(raises_debt) ((void)(raises_debt))
#endif

#ifdef __SDCC
#define heap_alignment() ((size_t)4u)
#define heap_overhead() ((size_t)8u)
#define heap_minimum_block() ((size_t)12u)
#define heap_maximum_physical() ((size_t)65532u)
#else
static size_t heap_alignment(void) {
  return 16u;
}

static size_t heap_overhead(void) { return sizeof(rift_physical_block); }

static size_t heap_minimum_block(void) {
  size_t raw = heap_overhead() + sizeof(rift_heap_link);
  size_t alignment = heap_alignment();
  return raw + (alignment - raw % alignment) % alignment;
}

static size_t heap_maximum_physical(void) {
  size_t maximum = (size_t)((rift_pool_offset_t)-1);
  return maximum - maximum % heap_alignment();
}
#endif

#ifndef __SDCC
static int checked_add(size_t left, size_t right, size_t *result) {
  if (left > SIZE_MAX - right) return 0;
  *result = left + right;
  return 1;
}
static int checked_round_up(size_t value, size_t alignment, size_t *result) {
  size_t remainder = value % alignment;
  if (remainder != 0) {
    size_t add = alignment - remainder;
    if (!checked_add(value, add, &value)) return 0;
  }
  *result = value;
  return 1;
}
#endif

#ifdef __SDCC
#define align_down(value, alignment) ((value) & (size_t)~((alignment)-1u))
#else
static size_t align_down(size_t value, size_t alignment) {
  return value - value % alignment;
}
#endif

#ifdef __SDCC
#define link_block(link) ((rift_physical_block *)(uintptr_t)(link))
#define block_link(block) ((rift_heap_link)(uintptr_t)(block))
#define block_payload(block) ((void *)(&(block)->public_header + 1))
#define payload_block(payload) \
  ((rift_physical_block *)((unsigned char *)(((rift_block_header *)(payload)) - 1) - \
                           offsetof(rift_physical_block, public_header)))
#define block_physical_size(block) \
  ((size_t)8u + (size_t)(block)->public_header.size)
#define block_is_free(block) ((block)->public_header.refcount == RIFT_RC_FREE)
#define block_previous_slot(block) ((rift_heap_link *)block_payload(block))
#define block_next_free(block) \
  ((rift_physical_block *)(uintptr_t)(block)->public_header.next_free)
#define block_previous_free(block) \
  ((rift_physical_block *)(uintptr_t)*block_previous_slot(block))
#define set_next_free(block, next) \
  ((block)->public_header.next_free = (rift_pool_offset_t)(uintptr_t)(next))
#define set_previous_free(block, previous) \
  (*block_previous_slot(block) = (rift_heap_link)(uintptr_t)(previous))
#else
static rift_physical_block *link_block(rift_heap_link link) {
  return link ? (rift_physical_block *)(uintptr_t)link : NULL;
}

static rift_heap_link block_link(const rift_physical_block *block) {
  return block ? (rift_heap_link)(uintptr_t)block : (rift_heap_link)0;
}

static void *block_payload(rift_physical_block *block) {
  return (void *)(&block->public_header + 1);
}

static rift_physical_block *payload_block(void *payload) {
  rift_block_header *header = ((rift_block_header *)payload) - 1;
  return (rift_physical_block *)((unsigned char *)header -
                                 offsetof(rift_physical_block, public_header));
}

static size_t block_physical_size(const rift_physical_block *block) {
  return heap_overhead() + (size_t)block->public_header.size;
}

static int block_is_free(const rift_physical_block *block) {
  return block->public_header.refcount == RIFT_RC_FREE;
}

static rift_heap_link *block_previous_slot(rift_physical_block *block) {
  return (rift_heap_link *)block_payload(block);
}

static rift_physical_block *block_next_free(const rift_physical_block *block) {
  return link_block((rift_heap_link)block->public_header.next_free);
}

static rift_physical_block *block_previous_free(rift_physical_block *block) {
  return link_block(*block_previous_slot(block));
}

static void set_next_free(rift_physical_block *block,
                          rift_physical_block *next) {
  block->public_header.next_free = (rift_pool_offset_t)block_link(next);
}

static void set_previous_free(rift_physical_block *block,
                              rift_physical_block *previous) {
  *block_previous_slot(block) = block_link(previous);
}
#endif

static void clear_free_links(rift_physical_block *block) {
  set_next_free(block, NULL);
  set_previous_free(block, NULL);
}

static unsigned int floor_log2_size(size_t value) {
  unsigned int result = 0;
  while (value > 1u) {
    value >>= 1;
    result++;
  }
  return result;
}

static size_t class_step(size_t base) {
#ifdef __SDCC
  return base < 32u ? 4u : base >> 3;
#else
  size_t step = base / RIFT_HEAP_SL_COUNT;
  if (step < heap_alignment()) step = heap_alignment();
  return step;
#endif
}

static size_t class_physical(unsigned int first, unsigned int second) {
  unsigned int fl = first + RIFT_HEAP_MIN_FL;
  size_t base = (size_t)1u << fl;
  if (first + 1u == RIFT_HEAP_FL_COUNT &&
      second + 1u == RIFT_HEAP_SL_COUNT)
    return heap_maximum_physical();
  return base + (size_t)second * class_step(base);
}

#ifndef __SDCC
static int map_size(size_t physical, int round_request,
                    rift_heap_class *mapped) {
  unsigned int fl;
  unsigned int first;
  size_t base;
  size_t step;
  size_t relative;
  size_t slot;
  if (physical < RIFT_HEAP_SMALLEST_MAPPABLE ||
      physical > heap_maximum_physical())
    return 0;
  fl = floor_log2_size(physical);
  if (fl < RIFT_HEAP_MIN_FL || fl - RIFT_HEAP_MIN_FL >= RIFT_HEAP_FL_COUNT)
    return 0;
  first = fl - RIFT_HEAP_MIN_FL;
  base = (size_t)1u << fl;
  step = class_step(base);
  relative = physical - base;
  if (round_request)
    slot = (relative + step - 1u) / step;
  else
    slot = relative / step;
  if (first + 1u == RIFT_HEAP_FL_COUNT) {
    if (physical == heap_maximum_physical())
      slot = RIFT_HEAP_SL_COUNT - 1u;
    else if (slot + 1u >= RIFT_HEAP_SL_COUNT) {
      if (round_request) {
        mapped->first = first;
        mapped->second = RIFT_HEAP_SL_COUNT - 1u;
        mapped->charged_physical = heap_maximum_physical();
        return 1;
      }
      slot = RIFT_HEAP_SL_COUNT - 2u;
    }
  }
  if (slot >= RIFT_HEAP_SL_COUNT) {
    first++;
    if (first >= RIFT_HEAP_FL_COUNT) return 0;
    slot = 0;
    base <<= 1;
    step = class_step(base);
  }
  mapped->first = first;
  mapped->second = (unsigned int)slot;
  mapped->charged_physical = class_physical(first, (unsigned int)slot);
  if (mapped->charged_physical > heap_maximum_physical()) return 0;
  return round_request ? mapped->charged_physical >= physical
                       : mapped->charged_physical <= physical;
}
#endif

#if defined(__SDCC) || defined(RIFT_ALLOCATOR_TEST)
static int map_size_zxn(size_t physical, int round_request,
                        rift_heap_class *mapped) {
  unsigned int fl;
  unsigned int first;
  unsigned int second;
  size_t base;
  size_t step;
  size_t relative;
  if (physical < 12u || physical > 65532u) return 0;
  if (physical < 32u) {
    first = physical >= 16u ? 1u : 0u;
    base = first ? 16u : 8u;
    second = (unsigned int)((physical - base) >> 2);
    mapped->first = (unsigned char)first;
    mapped->second = (unsigned char)second;
    mapped->charged_physical = physical;
    return 1;
  }
  fl = floor_log2_size(physical);
  first = fl - RIFT_HEAP_MIN_FL;
  base = (size_t)1u << fl;
  step = base >> 3;
  relative = physical - base;
  if (round_request) relative += step - 1u;
  second = (unsigned int)(relative >> (fl - 3u));
  if (first == 12u) {
    if (physical == 65532u)
      second = 7u;
    else if (second >= 7u) {
      if (round_request) {
        mapped->first = 12u;
        mapped->second = 7u;
        mapped->charged_physical = 65532u;
        return 1;
      }
      second = 6u;
    }
  } else if (second >= RIFT_HEAP_SL_COUNT) {
    first++;
    if (first >= 13u) return 0;
    second = 0;
    base <<= 1;
    step <<= 1;
  }
  mapped->first = (unsigned char)first;
  mapped->second = (unsigned char)second;
  mapped->charged_physical = first == 12u && second == 7u
                                  ? 65532u
                                  : base + (size_t)second * step;
  return round_request ? mapped->charged_physical >= physical
                       : mapped->charged_physical <= physical;
}

#ifdef RIFT_ALLOCATOR_TEST
static int map_size_zxn_reference(size_t physical, int round_request,
                                  rift_heap_class *mapped) {
  unsigned int fl = 0;
  unsigned int first;
  size_t base;
  size_t step;
  size_t relative;
  size_t slot;
  size_t value = physical;
  if (physical < 12u || physical > 65532u) return 0;
  while (value > 1u) {
    value >>= 1;
    fl++;
  }
  if (fl < 3u || fl > 15u) return 0;
  first = fl - 3u;
  base = (size_t)1u << fl;
  step = base < 32u ? 4u : base >> 3;
  relative = physical - base;
  slot = round_request ? (relative + step - 1u) / step : relative / step;
  if (first == 12u) {
    if (physical == 65532u)
      slot = 7u;
    else if (slot >= 7u) {
      if (round_request) {
        mapped->first = 12u;
        mapped->second = 7u;
        mapped->charged_physical = 65532u;
        return 1;
      }
      slot = 6u;
    }
  } else if (slot >= 8u) {
    first++;
    if (first >= 13u) return 0;
    slot = 0;
  }
  mapped->first = (unsigned char)first;
  mapped->second = (unsigned char)slot;
  if (first != fl - 3u) {
    base <<= 1;
    step <<= 1;
  }
  mapped->charged_physical = first == 12u && slot == 7u
                                  ? 65532u
                                  : base + slot * step;
  return round_request ? mapped->charged_physical >= physical
                       : mapped->charged_physical <= physical;
}
#endif
#endif

#ifdef __SDCC
#define map_request(physical, mapped) map_size_zxn((physical), 1, (mapped))
#define map_free(physical, mapped) map_size_zxn((physical), 0, (mapped))
#else
static int map_request(size_t physical, rift_heap_class *mapped) {
  return map_size(physical, 1, mapped);
}

static int map_free(size_t physical, rift_heap_class *mapped) {
  return map_size(physical, 0, mapped);
}
#endif

#ifdef __SDCC
#define bin_index(first, second) \
  ((size_t)(first) * RIFT_HEAP_SL_COUNT + (second))
#define bin_root(first, second) \
  ((rift_physical_block *)(uintptr_t)heap_roots[bin_index(first, second)])
#define set_bin_root(first, second, root) \
  (heap_roots[bin_index(first, second)] = (rift_heap_link)(uintptr_t)(root))
#else
static size_t bin_index(unsigned int first, unsigned int second) {
  return (size_t)first * RIFT_HEAP_SL_COUNT + second;
}

static rift_physical_block *bin_root(unsigned int first, unsigned int second) {
  return link_block(heap_roots[bin_index(first, second)]);
}

static void set_bin_root(unsigned int first, unsigned int second,
                         rift_physical_block *root) {
  heap_roots[bin_index(first, second)] = block_link(root);
}
#endif

static void set_bin_present(unsigned int first, unsigned int second) {
  heap_sl_bitmap[first] |= (unsigned char)(1u << second);
  heap_fl_bitmap |= (rift_heap_fl_bitmap)((rift_heap_fl_bitmap)1u << first);
}

static void clear_bin_if_empty(unsigned int first, unsigned int second) {
  if (bin_root(first, second)) return;
  heap_sl_bitmap[first] &= (unsigned char)~(1u << second);
  if (heap_sl_bitmap[first] == 0)
    heap_fl_bitmap &=
        (rift_heap_fl_bitmap)~((rift_heap_fl_bitmap)1u << first);
}

static void list_insert(rift_physical_block *block) {
  rift_heap_class mapped;
  rift_physical_block *head;
  size_t physical = block_physical_size(block);
  if (!map_free(physical, &mapped)) return;
  head = bin_root(mapped.first, mapped.second);
  set_previous_free(block, NULL);
  set_next_free(block, head);
  if (head) set_previous_free(head, block);
  set_bin_root(mapped.first, mapped.second, block);
  set_bin_present(mapped.first, mapped.second);
  heap_free_physical += physical;
  RIFT_HEAP_STAT_INC(free_bin_visits);
}

static void list_remove_known(rift_physical_block *block,
                              unsigned int first,
                              unsigned int second) {
  rift_physical_block *previous;
  rift_physical_block *next;
  size_t physical = block_physical_size(block);
  previous = block_previous_free(block);
  next = block_next_free(block);
  if (previous)
    set_next_free(previous, next);
  else
    set_bin_root(first, second, next);
  if (next) set_previous_free(next, previous);
  clear_free_links(block);
  clear_bin_if_empty(first, second);
  heap_free_physical -= physical;
  RIFT_HEAP_STAT_INC(free_bin_visits);
}

static void list_remove(rift_physical_block *block) {
  rift_heap_class mapped;
  if (!map_free(block_physical_size(block), &mapped)) return;
  list_remove_known(block, mapped.first, mapped.second);
}

static int first_set(unsigned int bits) {
  int index = 0;
  if (!bits) return -1;
  while ((bits & 1u) == 0) {
    bits >>= 1;
    index++;
  }
  return index;
}

#ifdef __SDCC
#define first_set_fl(bits) first_set((unsigned int)(bits))
#else
static int first_set_fl(rift_heap_fl_bitmap bits) {
  int index = 0;
  if (!bits) return -1;
  while ((bits & (rift_heap_fl_bitmap)1u) == 0) {
    bits >>= 1;
    index++;
  }
  return index;
}
#endif

static rift_physical_block *find_fit(const rift_heap_class *requested) {
  unsigned int first = requested->first;
  unsigned int second = requested->second;
  unsigned int higher_second;
  rift_heap_fl_bitmap higher_first;
  int next;
  rift_physical_block *block = bin_root(first, second);
  if (block) {
#ifdef __SDCC
    heap_selected_first = (unsigned char)first;
    heap_selected_second = (unsigned char)second;
#endif
    RIFT_HEAP_STAT_INC(allocation_bin_visits);
    return block;
  }
  higher_second = (unsigned int)heap_sl_bitmap[first] &
                  (unsigned int)(0xFFu << (second + 1u));
  next = first_set(higher_second);
  if (next >= 0) {
#ifdef __SDCC
    heap_selected_first = (unsigned char)first;
    heap_selected_second = (unsigned char)next;
#endif
    RIFT_HEAP_STAT_INC(allocation_bin_visits);
    return bin_root(first, (unsigned int)next);
  }
  if (first + 1u >= RIFT_HEAP_FL_COUNT) return NULL;
  higher_first = heap_fl_bitmap &
                 (rift_heap_fl_bitmap)(~(rift_heap_fl_bitmap)0u <<
                                       (first + 1u));
  next = first_set_fl(higher_first);
  if (next < 0) return NULL;
  first = (unsigned int)next;
  next = first_set((unsigned int)heap_sl_bitmap[first]);
  if (next < 0) return NULL;
#ifdef __SDCC
  heap_selected_first = (unsigned char)first;
  heap_selected_second = (unsigned char)next;
#endif
  RIFT_HEAP_STAT_INC(allocation_bin_visits);
  return bin_root(first, (unsigned int)next);
}

static void initialise_block(rift_physical_block *block,
                             size_t previous_physical,
                             size_t physical_size,
                             uint16_t refcount) {
  block->previous_physical_size = (rift_pool_offset_t)previous_physical;
  block->public_header.size =
      (rift_pool_offset_t)(physical_size - heap_overhead());
  block->public_header.refcount = refcount;
  block->public_header.next_free = RIFT_HEAP_LINK_NONE;
}

static rift_physical_block *next_block(rift_physical_block *block) {
  size_t offset = (size_t)((unsigned char *)block - heap_base);
  size_t physical = block_physical_size(block);
  if (offset + physical >= heap_committed_bytes) return NULL;
  return (rift_physical_block *)((unsigned char *)block + physical);
}

static int requested_aligned_physical(size_t payload_size,
                                      size_t *aligned_physical) {
  size_t requested;
  if (payload_size == 0) payload_size = 1;
#ifdef __SDCC
  if (payload_size > heap_maximum_physical() - heap_overhead() - 3u)
    return 0;
  requested = (heap_overhead() + payload_size + 3u) & (size_t)~3u;
#else
  if (!checked_add(heap_overhead(), payload_size, &requested) ||
      !checked_round_up(requested, heap_alignment(), &requested))
    return 0;
#endif
  if (requested < heap_minimum_block()) requested = heap_minimum_block();
  *aligned_physical = requested;
  return 1;
}

int rift_heap_init(void *base, size_t capacity, int permanent_limit) {
  size_t i;
  if (!base || heap_lifecycle != RIFT_HEAP_COLD ||
      ((uintptr_t)base % heap_alignment()) != 0)
    return 0;
  capacity = align_down(capacity, heap_alignment());
  if (capacity < heap_minimum_block() || capacity > heap_maximum_physical())
    return 0;
  heap_base = (unsigned char *)base;
  heap_capacity = capacity;
  heap_limit = capacity;
  heap_limit_permanent = permanent_limit ? 1u : 0u;
  heap_committed_bytes = 0;
  heap_live = 0;
  heap_peak_live = 0;
  heap_free_physical = 0;
  heap_tail = NULL;
  heap_fl_bitmap = 0;
#ifdef __SDCC
  heap_selected_first = 0;
  heap_selected_second = 0;
#endif
  for (i = 0; i < RIFT_HEAP_FL_COUNT; i++) heap_sl_bitmap[i] = 0;
  for (i = 0; i < RIFT_HEAP_BIN_COUNT; i++) heap_roots[i] = 0;
#ifdef RIFT_HEAP_ROUTINE_COMPACTION
  heap_compact_cursor.scan = RIFT_HEAP_CURSOR_NONE;
  heap_compact_cursor.hole = RIFT_HEAP_CURSOR_NONE;
  heap_compact_cursor.epoch = 0;
  heap_movement_epoch = 0;
  rift_managed_maintenance_due = 0;
#endif
#if RIFT_HEAP_STATS_ENABLED
  heap_stats = (rift_heap_stats){0};
#endif
  heap_lifecycle = RIFT_HEAP_READY;
  return 1;
}

#ifndef __SDCC
void rift_heap_deinit(void) {
  size_t i;
  heap_base = NULL;
  heap_capacity = 0;
  heap_limit = 0;
  heap_limit_permanent = 0;
  heap_committed_bytes = 0;
  heap_live = 0;
  heap_peak_live = 0;
  heap_free_physical = 0;
  heap_tail = NULL;
  heap_fl_bitmap = 0;
  for (i = 0; i < RIFT_HEAP_FL_COUNT; i++) heap_sl_bitmap[i] = 0;
  for (i = 0; i < RIFT_HEAP_BIN_COUNT; i++) heap_roots[i] = 0;
#ifdef RIFT_HEAP_ROUTINE_COMPACTION
  memset(&heap_compact_cursor, 0, sizeof(heap_compact_cursor));
  heap_movement_epoch = 0;
  rift_managed_maintenance_due = 0;
#endif
#if RIFT_HEAP_STATS_ENABLED
  heap_stats = (rift_heap_stats){0};
#endif
  heap_lifecycle = RIFT_HEAP_COLD;
}
#endif

#if !defined(__SDCC) || !defined(RIFT_ZXN_NO_BUMP_POOL)
int rift_heap_set_limit(size_t limit) {
  size_t old_limit;
  if (heap_lifecycle != RIFT_HEAP_READY) return 0;
  if (limit > heap_capacity) limit = heap_capacity;
  limit = align_down(limit, heap_alignment());
  if (heap_limit_permanent && limit != heap_limit) return 0;
  if (limit < heap_committed_bytes) return 0;
  old_limit = heap_limit;
  heap_limit = limit;
  if (old_limit != limit) note_layout_mutation(0);
  return 1;
}
#endif

void *rift_heap_try_alloc(size_t payload_size) {
  size_t aligned_physical;
  rift_heap_class requested;
  size_t physical_size;
  size_t uncommitted;
  rift_physical_block *block;
  size_t selected_size;
  if (heap_lifecycle != RIFT_HEAP_READY ||
      !requested_aligned_physical(payload_size, &aligned_physical)) {
    RIFT_HEAP_STAT_INC(capacity_failures);
    return NULL;
  }
  if (heap_fl_bitmap == 0) {
    uncommitted = heap_limit - heap_committed_bytes;
#ifdef __SDCC
    if (aligned_physical < 32u)
      physical_size = aligned_physical;
    else
#endif
    {
      if (!map_request(aligned_physical, &requested)) {
        RIFT_HEAP_STAT_INC(capacity_failures);
        return NULL;
      }
      physical_size = requested.charged_physical;
    }
    if (heap_limit_permanent && physical_size > uncommitted &&
        aligned_physical <= uncommitted)
      physical_size = uncommitted;
    if (physical_size <= uncommitted) {
      size_t previous = heap_tail ? block_physical_size(heap_tail) : 0;
      block = (rift_physical_block *)(heap_base + heap_committed_bytes);
      initialise_block(block, previous, physical_size, 1);
      heap_committed_bytes += physical_size;
      heap_tail = block;
      selected_size = physical_size;
      goto allocation_complete;
    }
    RIFT_HEAP_STAT_INC(capacity_failures);
    return NULL;
  }
  if (!map_request(aligned_physical, &requested)) {
    RIFT_HEAP_STAT_INC(capacity_failures);
    return NULL;
  }
  physical_size = requested.charged_physical;
  block = find_fit(&requested);
  if (block) {
    rift_physical_block *following;
    size_t previous = (size_t)block->previous_physical_size;
    size_t remainder;
    selected_size = block_physical_size(block);
#ifdef __SDCC
    list_remove_known(block, heap_selected_first, heap_selected_second);
#else
    list_remove(block);
#endif
    remainder = selected_size - physical_size;
    if (remainder >= heap_minimum_block()) {
      rift_physical_block *free_block =
          (rift_physical_block *)((unsigned char *)block + physical_size);
      initialise_block(block, previous, physical_size, 1);
      initialise_block(free_block, physical_size, remainder, RIFT_RC_FREE);
      clear_free_links(free_block);
      following = next_block(free_block);
      if (following)
        following->previous_physical_size = (rift_pool_offset_t)remainder;
      else
        heap_tail = free_block;
      list_insert(free_block);
      RIFT_HEAP_STAT_INC(splits);
      selected_size = physical_size;
    } else {
      initialise_block(block, previous, selected_size, 1);
    }
  } else {
    uncommitted = heap_limit - heap_committed_bytes;
    if (heap_limit_permanent && physical_size > uncommitted &&
        aligned_physical <= uncommitted)
      physical_size = uncommitted;
    if (physical_size <= uncommitted) {
      size_t previous = heap_tail ? block_physical_size(heap_tail) : 0;
      block = (rift_physical_block *)(heap_base + heap_committed_bytes);
      initialise_block(block, previous, physical_size, 1);
      heap_committed_bytes += physical_size;
      heap_tail = block;
      selected_size = physical_size;
    } else {
      size_t total_available = heap_free_physical + uncommitted;
      size_t largest_payload = rift_heap_largest_free_payload();
      size_t largest_physical =
          largest_payload ? largest_payload + heap_overhead() : 0;
      if (total_available >= physical_size &&
          largest_physical < physical_size)
        RIFT_HEAP_STAT_INC(fragmentation_failures);
      else
        RIFT_HEAP_STAT_INC(capacity_failures);
      return NULL;
    }
  }
allocation_complete:
  block->public_header.next_free = RIFT_HEAP_LINK_NONE;
  heap_live += selected_size;
  if (heap_live > heap_peak_live) heap_peak_live = heap_live;
  RIFT_HEAP_STAT_ADD(class_rounding_bytes,
                     physical_size - aligned_physical);
  note_layout_mutation(0);
  return block_payload(block);
}

static int validate_live_block(rift_physical_block *block,
                               rift_heap_free_result *result) {
  size_t offset;
  size_t physical;
  size_t previous_physical;
  rift_physical_block *next;
  rift_physical_block *previous = NULL;
  if ((unsigned char *)block < heap_base ||
      (unsigned char *)block >= heap_base + heap_committed_bytes ||
      ((uintptr_t)block % heap_alignment()) != 0) {
    *result = RIFT_HEAP_FREE_INVALID;
    return 0;
  }
  offset = (size_t)((unsigned char *)block - heap_base);
  physical = block_physical_size(block);
  if (physical < heap_minimum_block() || physical % heap_alignment() != 0 ||
      physical > heap_committed_bytes - offset || physical > heap_live) {
    *result = RIFT_HEAP_FREE_INVALID;
    return 0;
  }
  if (block_is_free(block)) {
    *result = RIFT_HEAP_FREE_DOUBLE;
    return 0;
  }
  if (block->public_header.refcount == RIFT_RC_STATIC) {
    *result = RIFT_HEAP_FREE_INVALID;
    return 0;
  }
  next = next_block(block);
  if (next && (size_t)next->previous_physical_size != physical) {
    *result = RIFT_HEAP_FREE_INVALID;
    return 0;
  }
  previous_physical = (size_t)block->previous_physical_size;
  if (previous_physical != 0) {
    if (previous_physical > offset ||
        previous_physical < heap_minimum_block() ||
        previous_physical % heap_alignment() != 0) {
      *result = RIFT_HEAP_FREE_INVALID;
      return 0;
    }
    previous =
        (rift_physical_block *)((unsigned char *)block - previous_physical);
    if (block_physical_size(previous) != previous_physical) {
      *result = RIFT_HEAP_FREE_INVALID;
      return 0;
    }
  } else if (offset != 0) {
    *result = RIFT_HEAP_FREE_INVALID;
    return 0;
  }
  *result = RIFT_HEAP_FREE_OK;
  return 1;
}

rift_heap_free_result rift_heap_try_free(void *payload) {
  uintptr_t payload_address;
  uintptr_t base_address;
  rift_physical_block *block;
  rift_physical_block *previous;
  rift_physical_block *next;
  rift_physical_block *following;
  rift_heap_free_result result;
#ifdef __SDCC
  size_t offset;
#endif
  size_t physical;
  size_t previous_physical;
  if (heap_lifecycle != RIFT_HEAP_READY || !payload)
    return RIFT_HEAP_FREE_INVALID;
  payload_address = (uintptr_t)payload;
  base_address = (uintptr_t)heap_base;
  if (payload_address < base_address ||
      payload_address - base_address >= heap_capacity)
    return RIFT_HEAP_FREE_NOT_OWNED;
  if (payload_address - base_address < heap_overhead() ||
      payload_address - base_address >= heap_committed_bytes)
    return RIFT_HEAP_FREE_INVALID;
  block = payload_block(payload);
#ifdef __SDCC
  if (block == heap_tail) {
    offset = (size_t)((unsigned char *)block - heap_base);
    physical = block_physical_size(block);
    if (((uintptr_t)block % heap_alignment()) != 0 ||
        physical < heap_minimum_block() ||
        physical % heap_alignment() != 0 ||
        physical != heap_committed_bytes - offset ||
        physical > heap_live)
      return RIFT_HEAP_FREE_INVALID;
    if (block_is_free(block)) return RIFT_HEAP_FREE_DOUBLE;
    if (block->public_header.refcount == RIFT_RC_STATIC)
      return RIFT_HEAP_FREE_INVALID;
    previous_physical = (size_t)block->previous_physical_size;
    if (previous_physical != 0) {
      if (previous_physical > offset ||
          previous_physical < heap_minimum_block() ||
          previous_physical % heap_alignment() != 0)
        return RIFT_HEAP_FREE_INVALID;
      previous =
          (rift_physical_block *)((unsigned char *)block - previous_physical);
      if (block_physical_size(previous) != previous_physical)
        return RIFT_HEAP_FREE_INVALID;
      if (block_is_free(previous)) goto general_free;
    } else {
      if (offset != 0) return RIFT_HEAP_FREE_INVALID;
      previous = NULL;
    }
    block->public_header.refcount = RIFT_RC_FREE;
    block->public_header.next_free = RIFT_HEAP_LINK_NONE;
    heap_live -= physical;
    heap_committed_bytes = offset;
    heap_tail = previous;
    note_layout_mutation(0);
    return RIFT_HEAP_FREE_OK;
  }
general_free:
#endif
  if (!validate_live_block(block, &result)) return result;
  physical = block_physical_size(block);
  previous_physical = (size_t)block->previous_physical_size;
  previous = previous_physical
                 ? (rift_physical_block *)((unsigned char *)block -
                                           previous_physical)
                 : NULL;
  next = next_block(block);
  heap_live -= physical;
  if (previous && block_is_free(previous)) {
    list_remove(previous);
    physical += block_physical_size(previous);
    block = previous;
    RIFT_HEAP_STAT_INC(coalesces);
  }
  if (next && block_is_free(next)) {
    list_remove(next);
    physical += block_physical_size(next);
    RIFT_HEAP_STAT_INC(coalesces);
  }
  previous_physical = (size_t)block->previous_physical_size;
  initialise_block(block, previous_physical, physical, RIFT_RC_FREE);
  clear_free_links(block);
  following = (rift_physical_block *)((unsigned char *)block + physical);
  if ((unsigned char *)following < heap_base + heap_committed_bytes) {
    following->previous_physical_size = (rift_pool_offset_t)physical;
    list_insert(block);
    note_layout_mutation(1);
  } else {
    heap_committed_bytes = (size_t)((unsigned char *)block - heap_base);
    if (heap_committed_bytes == 0)
      heap_tail = NULL;
    else
      heap_tail = (rift_physical_block *)((unsigned char *)block -
                                         previous_physical);
    note_layout_mutation(0);
  }
  return RIFT_HEAP_FREE_OK;
}

#ifdef RIFT_HEAP_ROUTINE_COMPACTION
#ifdef RIFT_HEAP_COMPACTION_STATS
rift_heap_compact_status rift_heap_private_compact_slice(
    rift_heap_compact_report *report) {
#else
rift_heap_compact_status rift_heap_private_compact_slice(void) {
#endif
  size_t scan;
  size_t hole_offset;
  unsigned char headers = 0;

#ifdef RIFT_HEAP_COMPACTION_STATS
  if (!report) return RIFT_HEAP_COMPACT_INVALID;
  memset(report, 0, sizeof(*report));
#endif
  if (heap_lifecycle != RIFT_HEAP_READY)
    return RIFT_HEAP_COMPACT_INVALID;
  if (rift_managed_maintenance_due == 0) return RIFT_HEAP_COMPACT_IDLE;
  if (heap_compact_cursor.scan == RIFT_HEAP_CURSOR_NONE ||
      heap_compact_cursor.epoch != heap_movement_epoch) {
    heap_compact_cursor.scan = 0;
    heap_compact_cursor.hole = RIFT_HEAP_CURSOR_NONE;
    heap_compact_cursor.epoch = heap_movement_epoch;
  }
  scan = heap_compact_cursor.scan;
  hole_offset = heap_compact_cursor.hole;

  while (headers < RIFT_HEAP_ROUTINE_HEADER_BUDGET) {
    rift_physical_block *source;
    size_t source_physical;

    if (scan >= heap_committed_bytes) goto pass_complete;
    if (scan % heap_alignment() != 0) return RIFT_HEAP_COMPACT_INVALID;
    source = (rift_physical_block *)(heap_base + scan);
    source_physical = block_physical_size(source);
    if (source_physical < heap_minimum_block() ||
        source_physical % heap_alignment() != 0 ||
        source_physical > heap_committed_bytes - scan)
      return RIFT_HEAP_COMPACT_INVALID;
    headers++;
    COMPACT_REPORT_INC(report, headers_scanned);
    if (block_is_free(source)) {
      if (hole_offset == RIFT_HEAP_CURSOR_NONE) hole_offset = scan;
      scan += source_physical;
      continue;
    }
    if (hole_offset == RIFT_HEAP_CURSOR_NONE) {
      scan += source_physical;
      continue;
    }

    {
      rift_heap_move_ticket ticket;
      rift_heap_move_class move_class = rift_managed_move_prepare(
          block_payload(source), source->public_header.next_free, &ticket);
      if (move_class == RIFT_HEAP_MOVE_INVALID)
        return RIFT_HEAP_COMPACT_INVALID;
      if (move_class != RIFT_HEAP_MOVE_MOVABLE ||
          source_physical > RIFT_HEAP_ROUTINE_COPY_BUDGET) {
        if (move_class == RIFT_HEAP_MOVE_PIN_BARRIER)
          COMPACT_REPORT_INC(report, pin_barriers);
        else if (move_class == RIFT_HEAP_MOVE_RAW_BARRIER)
          COMPACT_REPORT_INC(report, raw_barriers);
        else
          COMPACT_REPORT_INC(report, budget_barriers);
        hole_offset = RIFT_HEAP_CURSOR_NONE;
        scan += source_physical;
        continue;
      }

      {
        rift_physical_block *hole =
            (rift_physical_block *)(heap_base + hole_offset);
        rift_physical_block *following;
        rift_physical_block *free_block;
        size_t hole_physical = block_physical_size(hole);
        size_t following_physical = 0;
        size_t previous_physical;
        size_t free_physical;
        rift_pool_offset_t live_tag;
        void *old_raw_payload;

        if (!block_is_free(hole) || hole_offset + hole_physical != scan)
          return RIFT_HEAP_COMPACT_INVALID;
        previous_physical = (size_t)hole->previous_physical_size;
        live_tag = source->public_header.next_free;
        old_raw_payload = block_payload(source);
        following = next_block(source);
        if (following) {
          size_t following_offset =
              (size_t)((unsigned char *)following - heap_base);
          size_t validated_physical = block_physical_size(following);
          if (validated_physical < heap_minimum_block() ||
              validated_physical % heap_alignment() != 0 ||
              validated_physical >
                  heap_committed_bytes - following_offset)
            return RIFT_HEAP_COMPACT_INVALID;
          if (block_is_free(following)) {
            following_physical = validated_physical;
            list_remove(following);
            COMPACT_REPORT_INC(report, bin_edits);
          }
        }
        list_remove(hole);
        COMPACT_REPORT_INC(report, bin_edits);
        memmove(hole, source, source_physical);
        hole->previous_physical_size =
            (rift_pool_offset_t)previous_physical;
        COMPACT_REPORT_INC(report, boundary_writes);
        rift_managed_move_commit(old_raw_payload, block_payload(hole),
                                 live_tag, &ticket);

        free_physical = hole_physical + following_physical;
        free_block = (rift_physical_block *)((unsigned char *)hole +
                                             source_physical);
        initialise_block(free_block, source_physical, free_physical,
                         RIFT_RC_FREE);
        clear_free_links(free_block);
        COMPACT_REPORT_ADD(report, boundary_writes, 2u);
        scan += source_physical + following_physical;
        if (scan < heap_committed_bytes) {
          rift_physical_block *trailing =
              (rift_physical_block *)(heap_base + scan);
          trailing->previous_physical_size =
              (rift_pool_offset_t)free_physical;
          COMPACT_REPORT_INC(report, boundary_writes);
          list_insert(free_block);
          COMPACT_REPORT_INC(report, bin_edits);
          hole_offset =
              (size_t)((unsigned char *)free_block - heap_base);
        } else {
          heap_committed_bytes =
              (size_t)((unsigned char *)free_block - heap_base);
          heap_tail = hole;
          COMPACT_REPORT_ADD(report, frontier_writes, 2u);
          COMPACT_REPORT_ADD(report, tail_contracted, free_physical);
          scan = heap_committed_bytes;
          hole_offset = RIFT_HEAP_CURSOR_NONE;
        }
      }
      COMPACT_REPORT_INC(report, moves);
      COMPACT_REPORT_ADD(report, copied_bytes, source_physical);
#ifdef RIFT_HEAP_COMPACTION_STATS
      COMPACT_REPORT_ADD(report, index_reads, ticket.index_reads);
      COMPACT_REPORT_ADD(report, index_writes, ticket.index_writes);
#endif
      advance_movement_epoch();
      heap_compact_cursor.scan = scan;
      heap_compact_cursor.hole = hole_offset;
      heap_compact_cursor.epoch = heap_movement_epoch;
      if (scan >= heap_committed_bytes) goto pass_complete;
      return RIFT_HEAP_COMPACT_PROGRESS;
    }
  }

  heap_compact_cursor.scan = scan;
  heap_compact_cursor.hole = hole_offset;
  heap_compact_cursor.epoch = heap_movement_epoch;
  return RIFT_HEAP_COMPACT_PROGRESS;

pass_complete:
  heap_compact_cursor.scan = RIFT_HEAP_CURSOR_NONE;
  heap_compact_cursor.hole = RIFT_HEAP_CURSOR_NONE;
  heap_compact_cursor.epoch = heap_movement_epoch;
  rift_managed_maintenance_due = 0;
  return RIFT_HEAP_COMPACT_PASS_COMPLETE;
}

void rift_heap_movement_barrier_changed(void) {
  note_layout_mutation(1);
}

#ifdef RIFT_ALLOCATOR_TEST
uint16_t rift_heap_test_movement_epoch(void) {
  return heap_movement_epoch;
}

void rift_heap_test_set_movement_epoch(uint16_t epoch) {
  heap_movement_epoch = epoch;
}

size_t rift_heap_test_cursor_scan(void) {
  return heap_compact_cursor.scan;
}

unsigned int rift_heap_test_compaction_debt(void) {
  return rift_managed_maintenance_due;
}
#endif
#endif

#if !defined(__SDCC) || !defined(RIFT_ZXN_NO_BUMP_POOL)
size_t rift_heap_committed(void) { return heap_committed_bytes; }
#endif
size_t rift_heap_live_bytes(void) { return heap_live; }
size_t rift_heap_peak_live_bytes(void) { return heap_peak_live; }

size_t rift_heap_free_physical_bytes(void) {
  if (heap_lifecycle != RIFT_HEAP_READY) return 0;
  return heap_free_physical + (heap_limit - heap_committed_bytes);
}

size_t rift_heap_largest_free_payload(void) {
  size_t largest_physical = 0;
  size_t uncommitted;
  int first;
  int second;
  if (heap_lifecycle != RIFT_HEAP_READY) return 0;
  first = (int)RIFT_HEAP_FL_COUNT - 1;
  while (first >= 0 &&
         (heap_fl_bitmap &
          ((rift_heap_fl_bitmap)1u << (unsigned int)first)) == 0)
    first--;
  if (first >= 0) {
    second = (int)RIFT_HEAP_SL_COUNT - 1;
    while (second >= 0 &&
           (heap_sl_bitmap[first] &
            (unsigned char)(1u << (unsigned int)second)) == 0)
      second--;
    if (second >= 0)
      largest_physical =
          class_physical((unsigned int)first, (unsigned int)second);
  }
  uncommitted = align_down(heap_limit - heap_committed_bytes,
                           heap_alignment());
  if (uncommitted >= heap_minimum_block() &&
      uncommitted > largest_physical)
    largest_physical = uncommitted;
  return largest_physical >= heap_overhead()
             ? largest_physical - heap_overhead()
             : 0;
}

size_t rift_heap_block_overhead(void) { return heap_overhead(); }
size_t rift_heap_minimum_physical_block(void) { return heap_minimum_block(); }

#if !defined(__SDCC) || defined(RIFT_ALLOCATOR_STATS)
void rift_heap_stats_reset(void) {
#if RIFT_HEAP_STATS_ENABLED
  heap_stats = (rift_heap_stats){0};
#endif
}
rift_heap_stats rift_heap_stats_get(void) {
#if RIFT_HEAP_STATS_ENABLED
  return heap_stats;
#else
  rift_heap_stats result = {0};
  return result;
#endif
}
#endif

#ifdef RIFT_ALLOCATOR_TEST
int rift_heap_test_map_request(size_t physical_size,
                               unsigned int *first,
                               unsigned int *second,
                               size_t *charged_physical) {
  rift_heap_class mapped;
  if (!first || !second || !charged_physical ||
      !map_request(physical_size, &mapped))
    return 0;
  *first = mapped.first;
  *second = mapped.second;
  *charged_physical = mapped.charged_physical;
  return 1;
}

int rift_heap_test_map_free(size_t physical_size,
                            unsigned int *first,
                            unsigned int *second,
                            size_t *satisfiable_physical) {
  rift_heap_class mapped;
  if (!first || !second || !satisfiable_physical ||
      !map_free(physical_size, &mapped))
    return 0;
  *first = mapped.first;
  *second = mapped.second;
  *satisfiable_physical = mapped.charged_physical;
  return 1;
}

int rift_heap_test_compare_zxn_maps(size_t physical_size) {
  rift_heap_class optimized;
  rift_heap_class reference;
  if (map_size_zxn(physical_size, 1, &optimized) !=
      map_size_zxn_reference(physical_size, 1, &reference))
    return 0;
  if (optimized.first != reference.first ||
      optimized.second != reference.second ||
      optimized.charged_physical != reference.charged_physical)
    return 0;
  if (map_size_zxn(physical_size, 0, &optimized) !=
      map_size_zxn_reference(physical_size, 0, &reference))
    return 0;
  return optimized.first == reference.first &&
         optimized.second == reference.second &&
         optimized.charged_physical == reference.charged_physical;
}

int rift_heap_test_validate(void) {
  size_t offset = 0;
  size_t free_physical = 0;
  size_t list_count = 0;
  size_t physical_count = 0;
  unsigned int first;
  unsigned int second;
  rift_physical_block *previous_physical_block = NULL;
  if (heap_lifecycle != RIFT_HEAP_READY) return 0;
  while (offset < heap_committed_bytes) {
    rift_physical_block *block =
        (rift_physical_block *)(heap_base + offset);
    size_t physical = block_physical_size(block);
    if (physical < heap_minimum_block() ||
        physical % heap_alignment() != 0 ||
        physical > heap_committed_bytes - offset)
      return 0;
    if ((size_t)block->previous_physical_size !=
        (previous_physical_block
             ? block_physical_size(previous_physical_block)
             : 0))
      return 0;
    if (block_is_free(block)) {
      free_physical += physical;
      physical_count++;
      if (previous_physical_block &&
          block_is_free(previous_physical_block))
        return 0;
    }
    previous_physical_block = block;
    offset += physical;
  }
  if (previous_physical_block != heap_tail ||
      free_physical != heap_free_physical)
    return 0;
  for (first = 0; first < RIFT_HEAP_FL_COUNT; first++) {
    for (second = 0; second < RIFT_HEAP_SL_COUNT; second++) {
      rift_physical_block *block = bin_root(first, second);
      rift_physical_block *previous = NULL;
      int bitmap_present =
          (heap_sl_bitmap[first] & (unsigned char)(1u << second)) != 0;
      if ((block != NULL) != bitmap_present) return 0;
      while (block) {
        rift_heap_class mapped;
        if (!block_is_free(block) ||
            block_previous_free(block) != previous ||
            !map_free(block_physical_size(block), &mapped) ||
            mapped.first != first || mapped.second != second)
          return 0;
        previous = block;
        block = block_next_free(block);
        list_count++;
      }
    }
  }
  return list_count == physical_count;
}

size_t rift_heap_test_digest(void) {
  size_t hash = (size_t)1469598103934665603ull;
  size_t i;
  const unsigned char *bytes = (const unsigned char *)heap_roots;
  for (i = 0; i < sizeof(heap_roots); i++)
    hash = (hash ^ bytes[i]) * (size_t)1099511628211ull;
  bytes = (const unsigned char *)heap_sl_bitmap;
  for (i = 0; i < sizeof(heap_sl_bitmap); i++)
    hash = (hash ^ bytes[i]) * (size_t)1099511628211ull;
  hash ^= (size_t)heap_fl_bitmap;
  hash ^= heap_committed_bytes;
  hash ^= heap_live;
  hash ^= heap_free_physical;
  for (i = 0; i < heap_committed_bytes; i++)
    hash = (hash ^ heap_base[i]) * (size_t)1099511628211ull;
  return hash;
}

void rift_heap_test_poison_state(void) {
  memset(heap_roots, 0xA5, sizeof(heap_roots));
  memset(heap_sl_bitmap, 0xA5, sizeof(heap_sl_bitmap));
#if RIFT_HEAP_STATS_ENABLED
  memset(&heap_stats, 0xA5, sizeof(heap_stats));
#endif
  heap_base = (unsigned char *)(uintptr_t)0xA5A4u;
  heap_capacity = (size_t)0xA5A5u;
  heap_limit = (size_t)0xA5A5u;
  heap_limit_permanent = 0xA5u;
  heap_committed_bytes = (size_t)0xA5A5u;
  heap_live = (size_t)0xA5A5u;
  heap_peak_live = (size_t)0xA5A5u;
  heap_free_physical = (size_t)0xA5A5u;
  heap_tail = (rift_physical_block *)(uintptr_t)0xA5A4u;
  heap_fl_bitmap = (rift_heap_fl_bitmap)0xA5A5u;
#ifdef RIFT_HEAP_ROUTINE_COMPACTION
  memset(&heap_compact_cursor, 0xA5, sizeof(heap_compact_cursor));
  heap_movement_epoch = 0xA5A5u;
  rift_managed_maintenance_due = 0xA5u;
#endif
  heap_lifecycle = RIFT_HEAP_COLD;
}
#endif
