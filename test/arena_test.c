#include "lib/arena.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char *message) {
  if (condition)
    printf("PASS: %s\n", message);
  else {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void release_and_deinit(rift_arena_region *region) {
  rift_arena_release(region);
  rift_arena_deinit();
}

int main(void) {
  rift_arena_region region = {0};
  rift_arena_options options = {0};

  check(!rift_arena_acquire(16, &region),
        "an arena cannot be acquired before explicit initialization");
  check(!rift_arena_init(NULL),
        "initialization requires an explicit automatic-intent object");

  options.memory_max = 4096;
  options.memory_max_present = 1;
  check(rift_arena_init(&options) && rift_arena_acquire(0, &region) &&
            region.base && region.capacity == 4096,
        "a total maximum caps the acquired host region");
  {
    rift_arena_region second = {0};
    check(!rift_arena_acquire(4097, &second),
          "acquisition never returns less than its requested minimum");
  }
  release_and_deinit(&region);

  options = (rift_arena_options){
      .memory_min = 12 * 1024 * 1024,
      .memory_min_present = 1,
  };
  check(rift_arena_init(&options) && rift_arena_acquire(0, &region) &&
            region.capacity >= options.memory_min,
        "a total minimum raises automatic initial headroom");
  release_and_deinit(&region);

  options = (rift_arena_options){
      .memory_max = 4096,
      .memory_min = 8192,
      .memory_max_present = 1,
      .memory_min_present = 1,
  };
  check(!rift_arena_init(&options),
        "an impossible minimum and maximum are rejected atomically");
  rift_arena_deinit();

  options = (rift_arena_options){
      .memory_reserve = 0,
      .memory_reserve_present = 1,
  };
  check(!rift_arena_init(&options),
        "host initialization rejects even a literal-zero target reserve");
  rift_arena_deinit();

  options = (rift_arena_options){0};
  check(rift_arena_init(&options) && !rift_arena_init(&options),
        "double initialization cannot silently replace live arena intent");
  rift_arena_deinit();

  return failures ? 1 : 0;
}
