/*****************************************************
 * ROCK POOL RUNTIME — implementation
 * See pools.h for API; ADR-0003 §4, §8.5 for design.
 *****************************************************/

#include "pools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCK_HEADER_SIZE (sizeof(rock_block_header))

/* Size classes — payload sizes, excluding the 4-byte header.
 * Tunable per ADR-0003 §8.5; v1 uses power-of-two. */
static const size_t ROCK_CLASS_SIZES[] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
#define ROCK_NUM_CLASSES (sizeof(ROCK_CLASS_SIZES) / sizeof(ROCK_CLASS_SIZES[0]))

#ifdef __SDCC
/* The flat ZXN NEX image leaves no general-purpose z88dk malloc heap after
 * linking the full RTL. Keep the bounded Rock pools in BSS instead. */
#define ROCK_ZXN_BUMP_POOL_CAPACITY 2048
#define ROCK_ZXN_LONGLIVED_POOL_CAPACITY 2048
static char zxn_bump_pool[ROCK_ZXN_BUMP_POOL_CAPACITY];
static char zxn_longlived_pool[ROCK_ZXN_LONGLIVED_POOL_CAPACITY];
#endif

/* Bump pool. */
static char  *bump_base = NULL;
static size_t bump_top  = 0;
static size_t bump_cap  = 0;

/* Longlived pool. */
static char  *ll_base       = NULL;
static size_t ll_high_water = 0;
static size_t ll_cap        = 0;

/* Per-class freelist heads. Free blocks store a `next` pointer in their
 * payload bytes (smallest class is 8 bytes — fits any host pointer). */
static rock_block_header *class_freelists[ROCK_NUM_CLASSES];
/* "Miscellaneous" freelist: blocks whose size doesn't match a class
 * (e.g. result of reclaim() coalescing). */
static rock_block_header *misc_freelist = NULL;

static rock_oom_handler_fn oom_handler = NULL;

/* ---- OOM handling ---- */

static void default_oom(const char *pool_name, size_t requested, size_t available) {
  fprintf(stderr,
          "rock_pools: %s pool exhausted; requested %zu bytes, %zu available\n",
          pool_name, requested, available);
  exit(1);
}

void rock_set_oom_handler(rock_oom_handler_fn h) {
  oom_handler = h ? h : default_oom;
}

/* ---- Init / deinit ---- */

void rock_pools_init(size_t bump_capacity, size_t longlived_capacity) {
#ifdef __SDCC
  if (bump_capacity > sizeof(zxn_bump_pool) || longlived_capacity > sizeof(zxn_longlived_pool)) {
    fprintf(stderr, "rock_pools: requested ZXN pool capacity exceeds static budget\n");
    exit(1);
  }
  bump_base = zxn_bump_pool;
  ll_base = zxn_longlived_pool;
#else
  bump_base = (char *)malloc(bump_capacity);
  if (!bump_base) {
    fprintf(stderr, "rock_pools: failed to allocate %zu-byte bump pool\n",
            bump_capacity);
    exit(1);
  }
  bump_top = 0;
  bump_cap = bump_capacity;

  ll_base = (char *)malloc(longlived_capacity);
  if (!ll_base) {
    fprintf(stderr, "rock_pools: failed to allocate %zu-byte longlived pool\n",
            longlived_capacity);
    free(bump_base);
    exit(1);
  }
#endif
  bump_top = 0;
  bump_cap = bump_capacity;
  ll_high_water = 0;
  ll_cap = longlived_capacity;

  for (size_t i = 0; i < ROCK_NUM_CLASSES; i++) class_freelists[i] = NULL;
  misc_freelist = NULL;

  if (!oom_handler) oom_handler = default_oom;
}

void rock_pools_deinit(void) {
#ifndef __SDCC
  free(bump_base);
#endif
  bump_base = NULL;
  bump_top = 0;
  bump_cap = 0;

#ifndef __SDCC
  free(ll_base);
#endif
  ll_base = NULL;
  ll_high_water = 0;
  ll_cap = 0;

  for (size_t i = 0; i < ROCK_NUM_CLASSES; i++) class_freelists[i] = NULL;
  misc_freelist = NULL;
}

/* ---- Bump pool ---- */

void *rock_bump_alloc(size_t bytes) {
  /* Align allocations to 4 bytes so subsequent block headers are aligned. */
  size_t aligned = (bytes + 3u) & ~(size_t)3u;
  if (bump_top + aligned > bump_cap) {
    oom_handler("bump", aligned, bump_cap - bump_top);
    return NULL; /* unreachable when handler exits */
  }
  void *p = bump_base + bump_top;
  bump_top += aligned;
  return p;
}

rock_bump_mark rock_bump_save(void) { return bump_top; }

void rock_bump_restore(rock_bump_mark mark) {
  if (mark <= bump_top) bump_top = mark;
}

/* ---- Longlived pool ---- */

/* Free-link layout in payload bytes of a free block. */
typedef struct {
  rock_block_header *next;
} free_link_t;

/* Returns the freelist head pointer for a given block's size, choosing the
 * matching class freelist or the misc list. */
static rock_block_header **freelist_for_size(uint16_t size) {
  for (size_t i = 0; i < ROCK_NUM_CLASSES; i++) {
    if (ROCK_CLASS_SIZES[i] == size) return &class_freelists[i];
  }
  return &misc_freelist;
}

/* Round payload_size up to the smallest class that fits, or return the
 * exact payload_size if no class accommodates it (-> misc). */
static uint16_t round_to_class(size_t payload_size) {
  for (size_t i = 0; i < ROCK_NUM_CLASSES; i++) {
    if (payload_size <= ROCK_CLASS_SIZES[i]) return (uint16_t)ROCK_CLASS_SIZES[i];
  }
  return (uint16_t)payload_size;
}

static rock_block_header *freelist_pop(rock_block_header **list) {
  rock_block_header *head = *list;
  if (!head) return NULL;
  free_link_t *link = (free_link_t *)((char *)head + ROCK_HEADER_SIZE);
  *list = link->next;
  return head;
}

static void freelist_push(rock_block_header **list, rock_block_header *block) {
  block->refcount = ROCK_RC_FREE;
  free_link_t *link = (free_link_t *)((char *)block + ROCK_HEADER_SIZE);
  link->next = *list;
  *list = block;
}

/* Remove `block` from `list` if present. Returns 1 if removed, 0 if not found. */
static int freelist_remove(rock_block_header **list, rock_block_header *block) {
  rock_block_header *cur = *list;
  rock_block_header *prev = NULL;
  while (cur) {
    free_link_t *cur_link = (free_link_t *)((char *)cur + ROCK_HEADER_SIZE);
    if (cur == block) {
      if (prev) {
        free_link_t *prev_link = (free_link_t *)((char *)prev + ROCK_HEADER_SIZE);
        prev_link->next = cur_link->next;
      } else {
        *list = cur_link->next;
      }
      return 1;
    }
    prev = cur;
    cur = cur_link->next;
  }
  return 0;
}

void *rock_longlived_alloc(size_t payload_size) {
  /* Payload must be at least large enough to hold a free-link pointer. */
  if (payload_size < sizeof(free_link_t)) payload_size = sizeof(free_link_t);

  uint16_t alloc_size = round_to_class(payload_size);
  rock_block_header **flist = freelist_for_size(alloc_size);

  /* Try freelist reuse. */
  rock_block_header *block = freelist_pop(flist);
  if (block) {
    block->size = alloc_size;
    block->refcount = 1;
    return (char *)block + ROCK_HEADER_SIZE;
  }

  /* Carve from high water. */
  size_t total = ROCK_HEADER_SIZE + alloc_size;
  if (ll_high_water + total > ll_cap) {
    oom_handler("longlived", total, ll_cap - ll_high_water);
    return NULL; /* unreachable when handler exits */
  }
  block = (rock_block_header *)(ll_base + ll_high_water);
  block->size = alloc_size;
  block->refcount = 1;
  ll_high_water += total;
  return (char *)block + ROCK_HEADER_SIZE;
}

void rock_longlived_free(void *payload) {
  if (!payload) return;
  rock_block_header *block = ((rock_block_header *)payload) - 1;
  if (block->refcount == ROCK_RC_STATIC) return; /* static blocks are eternal */
  if (block->refcount == ROCK_RC_FREE) {
    fprintf(stderr, "rock_pools: double free at payload %p\n", payload);
    exit(1);
  }
  freelist_push(freelist_for_size(block->size), block);
}

void rock_longlived_reclaim(void) {
  size_t pos = 0;
  while (pos < ll_high_water) {
    rock_block_header *cur = (rock_block_header *)(ll_base + pos);
    size_t cur_total = ROCK_HEADER_SIZE + cur->size;
    size_t next_pos = pos + cur_total;

    if (cur->refcount == ROCK_RC_FREE && next_pos < ll_high_water) {
      rock_block_header *next = (rock_block_header *)(ll_base + next_pos);
      if (next->refcount == ROCK_RC_FREE) {
        /* Remove both from their freelists. */
        freelist_remove(freelist_for_size(cur->size), cur);
        freelist_remove(freelist_for_size(next->size), next);
        /* Merge: combined payload = cur.size + header + next.size. */
        size_t combined = (size_t)cur->size + ROCK_HEADER_SIZE + (size_t)next->size;
        cur->size = (uint16_t)combined;
        freelist_push(freelist_for_size(cur->size), cur);
        /* Stay at pos in case a third adjacent free exists. */
        continue;
      }
    }
    pos = next_pos;
  }
}

/* ---- Diagnostics ---- */

size_t rock_bump_used(void)            { return bump_top; }
size_t rock_bump_capacity(void)        { return bump_cap; }
size_t rock_longlived_used(void)       { return ll_high_water; }
size_t rock_longlived_capacity(void)   { return ll_cap; }

size_t rock_longlived_free_bytes(void) {
  size_t total = 0;
  size_t pos = 0;
  while (pos < ll_high_water) {
    rock_block_header *cur = (rock_block_header *)(ll_base + pos);
    if (cur->refcount == ROCK_RC_FREE) total += cur->size;
    pos += ROCK_HEADER_SIZE + cur->size;
  }
  return total;
}
