/*****************************************************
 * RIFT POOL RUNTIME
 * Temporary bounded allocator adapter over one managed arena region.
 *  - bump:      stack-disciplined save/restore from the high frontier.
 *  - longlived: refcount-managed buddy roots from the low frontier.
 *
 * Neither path owns a fixed capacity partition. Build-derived total arena
 * intent is passed through rift_arena_options; ZX Next otherwise uses its
 * complete checked linker gap.
 *****************************************************/

#ifndef RIFT_POOLS_H
#define RIFT_POOLS_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

/* Universal block header for refcount-managed allocations.
 *
 * `next_free` is an offset from the start of the long-lived pool. It is used
 * only while a block is free; the first offset-sized word of that free
 * payload holds its previous link. Live payload bytes remain wholly owned by
 * the caller. */
#ifdef __SDCC
typedef uint16_t rift_pool_offset_t;
#else
typedef size_t rift_pool_offset_t;
#endif

typedef struct rift_block_header {
  rift_pool_offset_t size; /* payload size in bytes (excluding this header) */
  uint16_t refcount; /* RIFT_RC_STATIC = static; RIFT_RC_FREE = free; else live count */
  rift_pool_offset_t next_free; /* pool offset, valid only while free */
} rift_block_header;

#define RIFT_RC_MAGAZINE 0xFFFDu
#define RIFT_RC_FREE    0xFFFEu
#define RIFT_RC_STATIC  0xFFFFu

/* Initialise the arena-backed allocator. Must be called before any other pool
 * API. On failure to obtain the target region, prints a diagnostic and exits. */
#if defined(__SDCC) && \
    (defined(RIFT_ZXN_NO_POOLS) || defined(RIFT_ZXN_TINY_CORE))
#define rift_pools_init(options) ((void)(options))
#define rift_pools_deinit() ((void)0)
#else
void rift_pools_init(const rift_arena_options *options);

/* ZX Next exits by halting the process, so teardown cannot return storage to
 * an enclosing allocator and is deliberately compiled away. */
#ifdef __SDCC
#define rift_pools_deinit() ((void)0)
#else
/* Tear down the host pools and release backing memory. */
void rift_pools_deinit(void);
#endif
#endif

/* ---- Bump pool ---- */

/* Allocate `bytes` raw bytes from the bump pool. On OOM, invokes the OOM
 * handler (which by default prints a diagnostic and exits). */
void *rift_bump_alloc(size_t bytes);

/* Bump save/restore for stack-disciplined region scopes. */
typedef size_t rift_bump_mark;

#if defined(__SDCC) && \
    (defined(RIFT_ZXN_NO_POOLS) || defined(RIFT_ZXN_TINY_CORE))
#define rift_bump_save() ((rift_bump_mark)0)
#define rift_bump_restore(mark) ((void)(mark))
#else
rift_bump_mark rift_bump_save(void);
void rift_bump_restore(rift_bump_mark mark);
#endif

/* ---- Longlived pool ---- */

/* Allocate a refcount-managed block. Returns a pointer to the payload.
 * The block header is at `((rift_block_header *)payload) - 1` with
 * a class-rounded size and refcount = 1. Allocation is a bounded magazine
 * pop or buddy-list split; it never searches heap blocks. */
void *rift_longlived_alloc(size_t payload_size);

/* Free a longlived block. Small blocks enter bounded magazines; other blocks
 * coalesce with their buddy immediately. Static blocks are ignored.
 * Double-free is a fatal diagnostic. */
void rift_longlived_free(void *payload);

/* Drain deferred small-object magazines into the buddy core. This is the
 * runtime entry point for Rift's explicit `collect;` safe point. Allocation
 * pressure invokes the same operation once automatically before reporting
 * OOM. */
void rift_collect(void);

/* ---- Diagnostics ---- */

size_t rift_bump_used(void);
size_t rift_bump_capacity(void);
size_t rift_longlived_used(void);
size_t rift_longlived_peak_used(void);
size_t rift_longlived_capacity(void);
size_t rift_longlived_free_bytes(void);
size_t rift_longlived_largest_free_block(void);

/* OOM handler signature. Default implementation prints a diagnostic to
 * stderr and exits(1). Tests may install a non-exiting handler. */
typedef void (*rift_oom_handler_fn)(const char *pool_name,
                                     size_t requested,
                                     size_t available);
void rift_set_oom_handler(rift_oom_handler_fn handler);

/* Bounded-work diagnostics for target profiling. Scan counters remain for
 * compatibility and must remain zero in the buddy allocator. */
typedef struct rift_allocator_stats {
  size_t allocations;
  size_t frees;
  size_t allocation_scan_steps;
  size_t free_scan_steps;
  size_t coalesces;
  size_t magazine_hits;
  size_t buddy_splits;
  size_t collect_calls;
} rift_allocator_stats;

void rift_allocator_stats_reset(void);
rift_allocator_stats rift_allocator_stats_get(void);

#endif /* RIFT_POOLS_H */
