#include "arena.h"

#include <stdint.h>

extern void *rift_zxn_arena_link_start(void);
extern void *rift_zxn_arena_link_end(void);

static rift_arena_region zxn_region;
#define RIFT_ZXN_ARENA_COLD 0xA5u
#define RIFT_ZXN_ARENA_READY 0x5Au
#define RIFT_ZXN_ARENA_ACQUIRED 0xC3u
static unsigned char zxn_state = RIFT_ZXN_ARENA_COLD;

static uintptr_t align_up(uintptr_t value, uintptr_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static uintptr_t align_down(uintptr_t value, uintptr_t alignment) {
  return value & ~(alignment - 1u);
}

int rift_arena_init(const rift_arena_options *options) {
  uintptr_t start;
  uintptr_t end;
  size_t capacity;
  if (!options || zxn_state != RIFT_ZXN_ARENA_COLD) return 0;
  start = align_up((uintptr_t)rift_zxn_arena_link_start(),
                   (uintptr_t)RIFT_ARENA_ALIGNMENT);
  end = (uintptr_t)rift_zxn_arena_link_end();
  if (options->memory_reserve > end) return 0;
  end = align_down(end - options->memory_reserve,
                   (uintptr_t)RIFT_ARENA_ALIGNMENT);
  if (start >= end) return 0;
  capacity = (size_t)(end - start);
  if (options->memory_max_present && capacity > options->memory_max) {
    capacity = options->memory_max;
    capacity -= capacity % RIFT_ARENA_ALIGNMENT;
  }
  if (capacity == 0 ||
      (options->memory_min_present && capacity < options->memory_min))
    return 0;
  zxn_region.base = (unsigned char *)start;
  zxn_region.capacity = capacity;
  zxn_state = RIFT_ZXN_ARENA_READY;
  return 1;
}

int rift_arena_acquire(size_t minimum_bytes, rift_arena_region *region) {
  if (zxn_state != RIFT_ZXN_ARENA_READY || !region ||
      minimum_bytes > zxn_region.capacity)
    return 0;
  *region = zxn_region;
  zxn_state = RIFT_ZXN_ARENA_ACQUIRED;
  return 1;
}

void rift_arena_release(rift_arena_region *region) {
  if (!region || !region->base) return;
  region->base = NULL;
  region->capacity = 0;
  zxn_state = RIFT_ZXN_ARENA_READY;
}

void rift_arena_deinit(void) {
  zxn_region.base = NULL;
  zxn_region.capacity = 0;
  zxn_state = RIFT_ZXN_ARENA_COLD;
}
