#include "pools.h"
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
#if RIFT_MEMORY_MAX_PRESENT
  if (rift_bump_capacity() != RIFT_MEMORY_MAX_VALUE) passed = 0;
#endif
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
  rift_collect();

  if (rift_longlived_used() != 0 || rift_bump_used() != 0) passed = 0;
  if (passed)
    zxn_test_pass();
  else
    zxn_test_fail();
  zxn_test_finish();
  rift_pools_deinit();
  return 0;
}
