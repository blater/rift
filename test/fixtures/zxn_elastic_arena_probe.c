#include "pools.h"
#include "segregated_heap.h"
#include "zxn_test.h"
#include <stdint.h>

extern void *rift_zxn_arena_link_start(void);
extern void *rift_zxn_arena_link_end(void);

int main(void) {
  uintptr_t raw_start = (uintptr_t)rift_zxn_arena_link_start();
  uintptr_t arena_start = (raw_start + 15u) & ~(uintptr_t)15u;
  uintptr_t raw_end = (uintptr_t)rift_zxn_arena_link_end();
  uintptr_t arena_end =
      (raw_end - RIFT_MEMORY_RESERVE_VALUE) & ~(uintptr_t)15u;
  rift_bump_mark mark;
  void *scratch;
  void *managed;
  void *exact_tail;
  void *exact_repeat;
  void *blocks[3];
  unsigned int request_first;
  unsigned int request_second;
  unsigned int free_first;
  unsigned int free_second;
  size_t charged_physical;
  size_t satisfiable_physical;
  int passed = 1;
  rift_arena_region direct_region = {0};
  rift_arena_options invalid_options = {
      .memory_reserve = 65534,
      .memory_reserve_present = 1,
  };
  rift_arena_options direct_options = {
      .memory_max = RIFT_MEMORY_MAX_VALUE,
      .memory_min = 16,
      .memory_reserve = RIFT_MEMORY_RESERVE_VALUE,
      .memory_max_present = RIFT_MEMORY_MAX_PRESENT,
      .memory_min_present = 1,
      .memory_reserve_present = RIFT_MEMORY_RESERVE_PRESENT,
  };
  rift_arena_options inconsistent_options = {
      .memory_max = 16,
      .memory_min = 32,
      .memory_max_present = 1,
      .memory_min_present = 1,
  };

  zxn_test_begin();
  if (sizeof(rift_block_header) != 6 ||
      rift_heap_block_overhead() != 8 ||
      rift_heap_minimum_physical_block() != 12)
    passed = 0;
  if (!rift_heap_test_map_request(65008u, &request_first, &request_second,
                                  &charged_physical) ||
      charged_physical != 65532u ||
      !rift_heap_test_map_free(charged_physical, &free_first, &free_second,
                               &satisfiable_physical) ||
      request_first != free_first || request_second != free_second ||
      satisfiable_physical != charged_physical)
    passed = 0;
  if (rift_arena_acquire(0, &direct_region)) passed = 0;
  if (rift_arena_init(NULL)) passed = 0;
  if (rift_arena_init(&invalid_options)) passed = 0;
  if (rift_arena_init(&inconsistent_options)) passed = 0;
  if (!rift_arena_init(&direct_options)) passed = 0;
  if (rift_arena_init(&direct_options)) passed = 0;
  if (rift_arena_acquire(0, NULL)) passed = 0;
  if (rift_arena_acquire((size_t)RIFT_MEMORY_MAX_VALUE + 1,
                         &direct_region))
    passed = 0;
  if (!rift_arena_acquire(0, &direct_region)) passed = 0;
  if (rift_arena_acquire(0, &direct_region)) passed = 0;
  rift_arena_release(&direct_region);
  if (!rift_arena_acquire(0, &direct_region)) passed = 0;
  rift_arena_release(&direct_region);
  rift_arena_deinit();

  rift_pools_init(NULL);
#if RIFT_MEMORY_MAX_PRESENT && defined(RIFT_ZXN_NO_BUMP_POOL)
  if (rift_bump_capacity() != 0) passed = 0;
  exact_tail = rift_longlived_alloc(RIFT_MEMORY_MAX_VALUE - 8u);
  if (!exact_tail || rift_longlived_used() != RIFT_MEMORY_MAX_VALUE)
    passed = 0;
  if (rift_heap_try_alloc(1) != NULL) passed = 0;
  rift_longlived_free(exact_tail);
  if (rift_longlived_used() != 0) passed = 0;
  exact_repeat = rift_longlived_alloc(RIFT_MEMORY_MAX_VALUE - 8u);
  if (exact_repeat != exact_tail ||
      rift_longlived_used() != RIFT_MEMORY_MAX_VALUE)
    passed = 0;
  if (rift_heap_try_alloc(1) != NULL) passed = 0;
  rift_longlived_free(exact_repeat);
  if (rift_longlived_used() != 0) passed = 0;
#endif

#if RIFT_MEMORY_MAX_PRESENT && !defined(RIFT_ZXN_NO_BUMP_POOL)
  mark = rift_bump_save();
  scratch = rift_bump_alloc(RIFT_MEMORY_MAX_VALUE - 464u);
  if (!scratch || rift_heap_try_alloc(456u) != NULL) passed = 0;
  rift_bump_restore(mark);
  managed = rift_longlived_alloc(456u);
  blocks[0] = rift_longlived_alloc(100u);
  if (!managed || !blocks[0]) passed = 0;
  rift_longlived_free(managed);
  exact_repeat = rift_longlived_alloc(456u);
  if (exact_repeat != managed) passed = 0;
  rift_longlived_free(exact_repeat);
  rift_longlived_free(blocks[0]);
  if (rift_longlived_used() != 0) passed = 0;
#endif
#ifndef RIFT_ZXN_NO_BUMP_POOL
  mark = rift_bump_save();
  scratch = rift_bump_alloc(4096);
  managed = rift_longlived_alloc(7000);
  if (!scratch || !managed) passed = 0;
  if ((uintptr_t)managed < arena_start || (uintptr_t)managed >= arena_end)
    passed = 0;
  if ((uintptr_t)scratch < arena_start || (uintptr_t)scratch >= arena_end)
    passed = 0;
  if ((uintptr_t)managed >= (uintptr_t)scratch) passed = 0;

  rift_longlived_free(managed);
  scratch = rift_bump_alloc(18000);
  if (!scratch) passed = 0;
  rift_bump_restore(mark);
  managed = rift_longlived_alloc(16000);
  if (!managed) passed = 0;
  rift_longlived_free(managed);
  blocks[0] = rift_longlived_alloc(100);
  blocks[1] = rift_longlived_alloc(200);
  blocks[2] = rift_longlived_alloc(300);
  if (!blocks[0] || !blocks[1] || !blocks[2]) passed = 0;
  rift_longlived_free(blocks[1]);
  managed = rift_longlived_alloc(180);
  if (managed != blocks[1]) passed = 0;
  rift_longlived_free(managed);
  rift_longlived_free(blocks[0]);
  rift_longlived_free(blocks[2]);
  rift_collect();
#endif

  if (rift_longlived_used() != 0 || rift_bump_used() != 0) passed = 0;
  if (passed)
    zxn_test_pass();
  else
    zxn_test_fail();
  zxn_test_finish();
  rift_pools_deinit();
  return 0;
}
