#ifndef RIFT_MANAGED_HEAP_H
#define RIFT_MANAGED_HEAP_H

#include "managed_ref.h"
#ifdef RIFT_HEAP_ROUTINE_COMPACTION
#include "segregated_heap_internal.h"
#endif

typedef enum managed_fault_reason {
  MANAGED_FAULT_CAPACITY = 1,
  MANAGED_FAULT_INVALID_REF,
  MANAGED_FAULT_REFCOUNT_OVERFLOW,
  MANAGED_FAULT_PIN_OVERFLOW,
  MANAGED_FAULT_DESTROYING,
  MANAGED_FAULT_LEAK,
  MANAGED_FAULT_INTERNAL
} managed_fault_reason;

typedef void (*managed_destructor_dispatch)(managed_type_id type,
                                            void *payload);
typedef void (*managed_fault_dispatch)(managed_fault_reason reason,
                                       size_t requested);

typedef struct managed_heap_options {
  managed_destructor_dispatch destroy;
  managed_fault_dispatch fault;
} managed_heap_options;

void managed_heap_init(const managed_heap_options *options)
    RIFT_MANAGED_FASTCALL;
void managed_heap_deinit(void);
size_t managed_heap_live_count(void);
size_t managed_heap_pin_count(void);

#if defined(RIFT_HEAP_ROUTINE_COMPACTION) && \
    defined(RIFT_HEAP_COMPACTION_STATS)
rift_heap_compact_status managed_heap_compact_routine(
    rift_heap_compact_report *report);
#endif

#ifdef RIFT_MANAGED_TEST
typedef struct managed_heap_test_stats {
  size_t cache_hits;
  size_t cache_misses;
  size_t node_visits;
  size_t node_bytes;
  size_t tree_height;
} managed_heap_test_stats;

managed_heap_test_stats managed_heap_test_stats_get(void);
size_t managed_heap_test_digest(void);
void managed_heap_test_fail_after(int successful_allocations);
uint16_t managed_heap_test_refcount(managed_ref ref);
unsigned int managed_heap_test_pin_count(managed_ref ref);
void managed_heap_test_set_counts(managed_ref ref, uint16_t refcount,
                                  unsigned int pin_count);
#endif

#if defined(RIFT_HEAP_ROUTINE_COMPACTION) && \
    (defined(RIFT_MANAGED_TEST) || defined(RIFT_ALLOCATOR_TEST))
void *managed_heap_test_root_address(void);
#endif

#endif
