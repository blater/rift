/*****************************************************
 * ROCK POOL RUNTIME
 * Two-pool allocator implementing ADR-0003.
 *  - bump:      stack-disciplined save/restore per scope.
 *  - longlived: refcount-managed magazines backed by fixed-order buddy lists.
 *****************************************************/

#ifndef ROCK_POOLS_H
#define ROCK_POOLS_H

#include <stddef.h>
#include <stdint.h>

#ifndef ROCK_ZXN_BUMP_POOL_CAPACITY
#define ROCK_ZXN_BUMP_POOL_CAPACITY 1024
#endif

#ifndef ROCK_ZXN_LONGLIVED_POOL_CAPACITY
#define ROCK_ZXN_LONGLIVED_POOL_CAPACITY 6144
#endif

/* Universal block header for refcount-managed allocations.
 *
 * `next_free` is an offset from the start of the long-lived pool. It is used
 * only while a block is free; the first offset-sized word of that free
 * payload holds its previous link. Live payload bytes remain wholly owned by
 * the caller. */
#ifdef __SDCC
typedef uint16_t rock_pool_offset_t;
#else
typedef size_t rock_pool_offset_t;
#endif

typedef struct rock_block_header {
  rock_pool_offset_t size; /* payload size in bytes (excluding this header) */
  uint16_t refcount; /* ROCK_RC_STATIC = static; ROCK_RC_FREE = free; else live count */
  rock_pool_offset_t next_free; /* pool offset, valid only while free */
} rock_block_header;

#define ROCK_RC_MAGAZINE 0xFFFDu
#define ROCK_RC_FREE    0xFFFEu
#define ROCK_RC_STATIC  0xFFFFu

/* Initialise the pools. Must be called before any other pool API.
 * On allocation failure to obtain backing memory, prints diagnostic and exits. */
#if defined(__SDCC) && defined(ROCK_ZXN_TINY_CORE)
#define rock_pools_init(bump_capacity, longlived_capacity) ((void)0)
#define rock_pools_deinit() ((void)0)
#else
void rock_pools_init(size_t bump_capacity, size_t longlived_capacity);

/* Tear down the pools. Frees backing memory. */
void rock_pools_deinit(void);
#endif

/* ---- Bump pool ---- */

/* Allocate `bytes` raw bytes from the bump pool. On OOM, invokes the OOM
 * handler (which by default prints a diagnostic and exits). */
void *rock_bump_alloc(size_t bytes);

/* Bump save/restore for stack-disciplined region scopes. */
typedef size_t rock_bump_mark;

#if defined(__SDCC) && defined(ROCK_ZXN_TINY_CORE)
#define rock_bump_save() ((rock_bump_mark)0)
#define rock_bump_restore(mark) ((void)(mark))
#else
rock_bump_mark rock_bump_save(void);
void rock_bump_restore(rock_bump_mark mark);
#endif

/* ---- Longlived pool ---- */

/* Allocate a refcount-managed block. Returns a pointer to the payload.
 * The block header is at `((rock_block_header *)payload) - 1` with
 * a class-rounded size and refcount = 1. Allocation is a bounded magazine
 * pop or buddy-list split; it never searches heap blocks. */
void *rock_longlived_alloc(size_t payload_size);

/* Free a longlived block. Small blocks enter bounded magazines; other blocks
 * coalesce with their buddy immediately. Static blocks are ignored.
 * Double-free is a fatal diagnostic. */
void rock_longlived_free(void *payload);

/* Drain deferred small-object magazines into the buddy core. This is the
 * runtime entry point for Rock's explicit `collect;` safe point. */
void rock_collect(void);

/* Compatibility spelling for the old runtime API. */
void rock_longlived_reclaim(void);

/* ---- Diagnostics ---- */

size_t rock_bump_used(void);
size_t rock_bump_capacity(void);
size_t rock_longlived_used(void);
size_t rock_longlived_peak_used(void);
size_t rock_longlived_capacity(void);
size_t rock_longlived_free_bytes(void);
size_t rock_longlived_largest_free_block(void);

/* OOM handler signature. Default implementation prints a diagnostic to
 * stderr and exits(1). Tests may install a non-exiting handler. */
typedef void (*rock_oom_handler_fn)(const char *pool_name,
                                     size_t requested,
                                     size_t available);
void rock_set_oom_handler(rock_oom_handler_fn handler);

/* Bounded-work diagnostics for target profiling. Scan counters remain for
 * compatibility and must remain zero in the buddy allocator. */
typedef struct rock_allocator_stats {
  size_t allocations;
  size_t frees;
  size_t allocation_scan_steps;
  size_t free_scan_steps;
  size_t coalesces;
  size_t magazine_hits;
  size_t buddy_splits;
  size_t collect_calls;
} rock_allocator_stats;

void rock_allocator_stats_reset(void);
rock_allocator_stats rock_allocator_stats_get(void);

#endif /* ROCK_POOLS_H */
