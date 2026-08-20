#ifndef RIFT_SEGREGATED_HEAP_H
#define RIFT_SEGREGATED_HEAP_H

#include <stddef.h>

typedef enum rift_heap_free_result {
  RIFT_HEAP_FREE_OK = 0,
  RIFT_HEAP_FREE_INVALID,
  RIFT_HEAP_FREE_DOUBLE,
  RIFT_HEAP_FREE_NOT_OWNED
} rift_heap_free_result;

typedef struct rift_heap_stats {
  size_t allocation_bin_visits;
  size_t free_bin_visits;
  size_t splits;
  size_t coalesces;
  size_t class_rounding_bytes;
  size_t capacity_failures;
  size_t fragmentation_failures;
} rift_heap_stats;

/* `permanent_limit` permits the exact whole-tail escape and forbids later
 * limit changes; expandable bump-sharing heaps keep every block class-sized. */
int rift_heap_init(void *base, size_t capacity, int permanent_limit);
void rift_heap_deinit(void);
int rift_heap_set_limit(size_t limit);
void *rift_heap_try_alloc(size_t payload_size);
rift_heap_free_result rift_heap_try_free(void *payload);

size_t rift_heap_committed(void);
size_t rift_heap_live_bytes(void);
size_t rift_heap_peak_live_bytes(void);
size_t rift_heap_free_physical_bytes(void);
size_t rift_heap_largest_free_payload(void);
size_t rift_heap_block_overhead(void);
size_t rift_heap_minimum_physical_block(void);

void rift_heap_stats_reset(void);
rift_heap_stats rift_heap_stats_get(void);

#ifdef RIFT_ALLOCATOR_TEST
int rift_heap_test_map_request(size_t physical_size,
                               unsigned int *first,
                               unsigned int *second,
                               size_t *charged_physical);
int rift_heap_test_map_free(size_t physical_size,
                            unsigned int *first,
                            unsigned int *second,
                            size_t *satisfiable_physical);
int rift_heap_test_compare_zxn_maps(size_t physical_size);
int rift_heap_test_validate(void);
size_t rift_heap_test_digest(void);
void rift_heap_test_poison_state(void);
#endif

#endif
