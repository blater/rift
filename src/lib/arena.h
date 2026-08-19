#ifndef RIFT_ARENA_H
#define RIFT_ARENA_H

#include <stddef.h>

#define RIFT_ARENA_ALIGNMENT 16u

typedef struct rift_arena_options {
  size_t memory_max;
  size_t memory_min;
  size_t memory_reserve;
  unsigned char memory_max_present;
  unsigned char memory_min_present;
  unsigned char memory_reserve_present;
} rift_arena_options;

typedef struct rift_arena_region {
  unsigned char *base;
  size_t capacity;
} rift_arena_region;

/* Initialise target address-space policy explicitly after CRT startup. */
int rift_arena_init(const rift_arena_options *options);

/* Acquire one fungible managed region with at least minimum_bytes capacity.
 * A backend may return more. Regions contain no allocator-owned partition. */
int rift_arena_acquire(size_t minimum_bytes, rift_arena_region *region);
void rift_arena_release(rift_arena_region *region);
void rift_arena_deinit(void);

#endif
