#include "arena.h"

#include <stdint.h>
#include <stdlib.h>

static rift_arena_options host_options;
static size_t host_acquired_bytes;
static int host_initialised;

#define RIFT_HOST_DEFAULT_REGION_BYTES (8u * 1024u * 1024u)

static int checked_add(size_t left, size_t right, size_t *result) {
  if (left > SIZE_MAX - right) return 0;
  *result = left + right;
  return 1;
}

int rift_arena_init(const rift_arena_options *options) {
  if (!options || host_initialised) return 0;
  if (options->memory_reserve_present) return 0;
  if (options->memory_max_present && options->memory_max == 0) return 0;
  if (options->memory_max_present && options->memory_min_present &&
      options->memory_min > options->memory_max)
    return 0;
  host_options = *options;
  host_acquired_bytes = 0;
  host_initialised = 1;
  return 1;
}

int rift_arena_acquire(size_t minimum_bytes, rift_arena_region *region) {
  size_t desired;
  size_t new_total;
  unsigned char *base;
  if (!host_initialised || !region) return 0;
  region->base = NULL;
  region->capacity = 0;
  desired = minimum_bytes;
  if (host_acquired_bytes == 0 && desired < RIFT_HOST_DEFAULT_REGION_BYTES)
    desired = RIFT_HOST_DEFAULT_REGION_BYTES;
  if (host_acquired_bytes == 0 && host_options.memory_min_present &&
      desired < host_options.memory_min)
    desired = host_options.memory_min;
  if (host_options.memory_max_present) {
    size_t remaining = host_options.memory_max - host_acquired_bytes;
    if (minimum_bytes > remaining) return 0;
    if (host_acquired_bytes == 0 && desired > remaining) desired = remaining;
    if (desired > remaining) return 0;
  }
  if (desired == 0) return 0;
  if (!checked_add(host_acquired_bytes, desired, &new_total)) return 0;
  base = (unsigned char *)malloc(desired);
  if (!base) return 0;
  host_acquired_bytes = new_total;
  region->base = base;
  region->capacity = desired;
  return 1;
}

void rift_arena_release(rift_arena_region *region) {
  if (!region || !region->base) return;
  free(region->base);
  if (region->capacity <= host_acquired_bytes)
    host_acquired_bytes -= region->capacity;
  else
    host_acquired_bytes = 0;
  region->base = NULL;
  region->capacity = 0;
}

void rift_arena_deinit(void) {
  host_options = (rift_arena_options){0};
  host_acquired_bytes = 0;
  host_initialised = 0;
}
