/*****************************************************
 * Tests for the Rift pool runtime (src/lib/pools.{h,c}).
 * Standalone C harness — no Rift-language integration yet.
 *****************************************************/

#include "../src/lib/pools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUMP_CAP   (1u * 1024u * 1024u)
#define LL_CAP     (1u * 1024u * 1024u)

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define EXPECT(cond, msg)                                                      \
  do {                                                                          \
    if (!(cond)) {                                                              \
      fprintf(stderr, "  FAIL: %s (%s)\n", msg, #cond);                         \
      tests_failed++;                                                           \
      return;                                                                   \
    }                                                                           \
  } while (0)

#define RUN(fn)                                                                 \
  do {                                                                          \
    tests_run++;                                                                \
    int before = tests_failed;                                                  \
    fn();                                                                       \
    if (tests_failed == before) {                                               \
      tests_passed++;                                                           \
      printf("  ok %s\n", #fn);                                                 \
    } else {                                                                    \
      printf("  FAIL %s\n", #fn);                                               \
    }                                                                           \
  } while (0)

/* ---- Bump pool ---- */

static void bump_alloc_returns_nonnull(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p = rift_bump_alloc(64);
  EXPECT(p != NULL, "bump_alloc returned NULL");
  rift_pools_deinit();
}

static void bump_alloc_advances_top(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  size_t before = rift_bump_used();
  rift_bump_alloc(64);
  EXPECT(rift_bump_used() == before + 64, "bump top did not advance by 64");
  rift_pools_deinit();
}

static void bump_save_restore(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  rift_bump_alloc(32);
  rift_bump_mark mark = rift_bump_save();
  rift_bump_alloc(128);
  rift_bump_alloc(64);
  EXPECT(rift_bump_used() == 32 + 128 + 64, "bump top wrong before restore");
  rift_bump_restore(mark);
  EXPECT(rift_bump_used() == 32, "bump top did not restore to mark");
  rift_pools_deinit();
}

static void bump_save_restore_nested(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  rift_bump_mark m0 = rift_bump_save();
  rift_bump_alloc(16);
  rift_bump_mark m1 = rift_bump_save();
  rift_bump_alloc(32);
  rift_bump_mark m2 = rift_bump_save();
  rift_bump_alloc(64);
  rift_bump_restore(m2);
  EXPECT(rift_bump_used() == 16 + 32, "inner restore wrong");
  rift_bump_restore(m1);
  EXPECT(rift_bump_used() == 16, "middle restore wrong");
  rift_bump_restore(m0);
  EXPECT(rift_bump_used() == 0, "outer restore wrong");
  rift_pools_deinit();
}

static void bump_alignment(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  /* Allocate odd sizes; every result must meet the runtime alignment. */
  void *a = rift_bump_alloc(1);
  void *b = rift_bump_alloc(1);
  void *c = rift_bump_alloc(1);
  EXPECT(((uintptr_t)a % sizeof(void *)) == 0, "first bump allocation is misaligned");
  EXPECT(((uintptr_t)b % sizeof(void *)) == 0, "second bump allocation is misaligned");
  EXPECT(((uintptr_t)c % sizeof(void *)) == 0, "third bump allocation is misaligned");
  rift_pools_deinit();
}

/* ---- Longlived pool ---- */

static void longlived_alloc_returns_payload(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p = rift_longlived_alloc(16);
  EXPECT(p != NULL, "longlived_alloc returned NULL");
  rift_block_header *h = ((rift_block_header *)p) - 1;
  EXPECT(h->refcount == 1, "fresh block refcount must be 1");
  EXPECT(h->size >= 16, "block size below requested");
  rift_pools_deinit();
}

static void longlived_size_rounds_to_alignment(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p = rift_longlived_alloc(20);
  rift_block_header *h = ((rift_block_header *)p) - 1;
  EXPECT(h->size >= 20, "size is below request");
  EXPECT((h->size % sizeof(void *)) == 0, "size is not allocation-aligned");
  rift_pools_deinit();
}

static void longlived_free_marks_free(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p = rift_longlived_alloc(16);
  rift_longlived_free(p);
  rift_block_header *h = ((rift_block_header *)p) - 1;
  EXPECT(h->refcount == RIFT_RC_MAGAZINE || h->refcount == RIFT_RC_FREE,
         "freed block must enter a magazine or buddy free list");
  rift_pools_deinit();
}

static void longlived_free_then_realloc_reuses(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rift_longlived_alloc(16);
  rift_longlived_free(p1);
  void *p2 = rift_longlived_alloc(16);
  EXPECT(p1 == p2, "freed block was not reused on subsequent same-size alloc");
  rift_pools_deinit();
}

static void longlived_static_sentinel_not_freed(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p = rift_longlived_alloc(16);
  rift_block_header *h = ((rift_block_header *)p) - 1;
  h->refcount = RIFT_RC_STATIC;
  rift_longlived_free(p); /* should be a no-op */
  EXPECT(h->refcount == RIFT_RC_STATIC, "static block was modified by free");
  rift_pools_deinit();
}

static void longlived_payload_writable(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  char *p = (char *)rift_longlived_alloc(64);
  memset(p, 0xAB, 64);
  for (int i = 0; i < 64; i++) {
    EXPECT((unsigned char)p[i] == 0xAB, "payload byte not writable");
  }
  rift_pools_deinit();
}

static void longlived_payload_alignment(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *a = rift_longlived_alloc(1);
  void *b = rift_longlived_alloc(17);
  void *c = rift_longlived_alloc(63);
  EXPECT(((uintptr_t)a % sizeof(void *)) == 0, "first longlived payload is misaligned");
  EXPECT(((uintptr_t)b % sizeof(void *)) == 0, "second longlived payload is misaligned");
  EXPECT(((uintptr_t)c % sizeof(void *)) == 0, "third longlived payload is misaligned");
  rift_pools_deinit();
}

/* ---- Reclaim ---- */

static void reclaim_drains_magazines_and_coalesces_buddies(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rift_longlived_alloc(16);
  void *p2 = rift_longlived_alloc(16);
  rift_longlived_free(p1);
  rift_longlived_free(p2);
  rift_longlived_reclaim();
  EXPECT(rift_longlived_largest_free_block() >= 32,
         "collection did not make a larger buddy class available");
  rift_pools_deinit();
}

static void reclaim_does_not_merge_live_with_free(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rift_longlived_alloc(16);
  void *p2 = rift_longlived_alloc(16);
  void *p3 = rift_longlived_alloc(16);
  rift_longlived_free(p1);
  rift_longlived_free(p3);
  /* p2 is live between two free blocks; reclaim must not merge across it. */
  rift_longlived_reclaim();
  rift_block_header *h1 = ((rift_block_header *)p1) - 1;
  rift_block_header *h2 = ((rift_block_header *)p2) - 1;
  rift_block_header *h3 = ((rift_block_header *)p3) - 1;
  EXPECT(h2->refcount == 1, "live block was disturbed by reclaim");
  EXPECT(h1->refcount == RIFT_RC_FREE || h1->refcount == RIFT_RC_MAGAZINE,
         "first free block was disturbed");
  EXPECT(h3->refcount == RIFT_RC_FREE || h3->refcount == RIFT_RC_MAGAZINE,
         "last free block was disturbed");
  rift_pools_deinit();
}

static void reclaim_chains_three_adjacent(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rift_longlived_alloc(16);
  void *p2 = rift_longlived_alloc(16);
  void *p3 = rift_longlived_alloc(16);
  rift_longlived_free(p1);
  rift_longlived_free(p2);
  rift_longlived_free(p3);
  rift_longlived_reclaim();
  EXPECT(rift_longlived_largest_free_block() >= 32,
         "three frees did not restore a useful buddy class");
  rift_pools_deinit();
}

static void coalesced_region_serves_smaller_allocation(void) {
  rift_pools_init(BUMP_CAP, 256);
  void *p1 = rift_longlived_alloc(16);
  void *p2 = rift_longlived_alloc(16);
  void *p3 = rift_longlived_alloc(16);
  rift_longlived_free(p1);
  rift_longlived_free(p2);
  rift_longlived_free(p3);
  EXPECT(rift_longlived_largest_free_block() >= 48,
         "three freed blocks were not coalesced into a reusable region");
  void *reuse = rift_longlived_alloc(32);
  EXPECT(reuse != NULL, "smaller allocation did not use recovered capacity");
  rift_pools_deinit();
}

static void common_short_lived_workload_has_constant_scan_depth(void) {
  rift_pools_init(1024, 6144);
  rift_allocator_stats_reset();
  for (int i = 0; i < 50; i++) {
    void *number = rift_longlived_alloc(8);
    void *message = rift_longlived_alloc(16);
    rift_longlived_free(message);
    rift_longlived_free(number);
  }
  rift_allocator_stats stats = rift_allocator_stats_get();
  EXPECT(stats.allocations == 100, "short-lived workload allocation count is wrong");
  EXPECT(stats.allocation_scan_steps == 0 && stats.free_scan_steps == 0,
         "normal workload must not scan heap blocks");
  EXPECT(stats.magazine_hits >= 90,
         "short-lived workload did not remain on the magazine fast path");
  rift_pools_deinit();
}

static void hot_small_block_cache_avoids_a_list_scan(void) {
  rift_pools_init(1024, 1024);
  void *before = rift_longlived_alloc(16);
  void *hot = rift_longlived_alloc(8);
  void *after = rift_longlived_alloc(16);
  rift_longlived_free(hot);
  rift_allocator_stats_reset();
  void *reuse = rift_longlived_alloc(8);
  rift_allocator_stats stats = rift_allocator_stats_get();
  EXPECT(reuse == hot, "hot cache did not reuse the recently freed small block");
  EXPECT(stats.allocation_scan_steps == 0,
         "hot-cache allocation should not traverse the free list");
  rift_longlived_free(before);
  rift_longlived_free(reuse);
  rift_longlived_free(after);
  rift_pools_deinit();
}

static void collect_reclaims_magazines_for_a_larger_class(void) {
  rift_pools_init(1024, 128);
  void *blocks[4];
  for (int i = 0; i < 4; i++) blocks[i] = rift_longlived_alloc(1);
  for (int i = 0; i < 4; i++) rift_longlived_free(blocks[i]);
  EXPECT(rift_longlived_largest_free_block() < 16,
         "magazine-only capacity must not look like a larger class");
  rift_allocator_stats_reset();
  rift_collect();
  rift_allocator_stats stats = rift_allocator_stats_get();
  EXPECT(stats.collect_calls == 1, "explicit collection was not counted");
  EXPECT(rift_longlived_largest_free_block() >= 16,
         "collection did not coalesce cached blocks into a larger class");
  EXPECT(rift_longlived_alloc(16) != NULL,
         "larger allocation did not use collected capacity");
  rift_pools_deinit();
}

static void fragmented_large_request_uses_bounded_buddy_classes(void) {
  rift_pools_init(1024, 1024);
  void *blocks[16];
  for (int i = 0; i < 16; i++) blocks[i] = rift_longlived_alloc(16);
  for (int i = 0; i < 16; i += 2) rift_longlived_free(blocks[i]);
  rift_allocator_stats_reset();
  void *large = rift_longlived_alloc(24);
  rift_allocator_stats stats = rift_allocator_stats_get();
  EXPECT(large != NULL, "fragmented fallback allocation failed unexpectedly");
  EXPECT(stats.allocation_scan_steps == 0 && stats.free_scan_steps == 0,
         "fragmented allocation must not inspect heap blocks");
  rift_longlived_free(large);
  for (int i = 1; i < 16; i += 2) rift_longlived_free(blocks[i]);
  rift_pools_deinit();
}

/* ---- OOM ---- */

static int          oom_called = 0;
static const char  *oom_pool   = NULL;
static size_t       oom_requested = 0;

static void capture_oom(const char *pool, size_t requested, size_t avail) {
  (void)avail;
  oom_called = 1;
  oom_pool = pool;
  oom_requested = requested;
}

static void bump_oom_invokes_handler(void) {
  rift_pools_init(64, LL_CAP);
  oom_called = 0; oom_pool = NULL;
  rift_set_oom_handler(capture_oom);
  rift_bump_alloc(128); /* exceeds cap */
  EXPECT(oom_called == 1, "bump OOM handler not invoked");
  EXPECT(oom_pool != NULL && strcmp(oom_pool, "bump") == 0, "wrong pool name");
  rift_pools_deinit();
  rift_set_oom_handler(NULL);
}

static void longlived_oom_invokes_handler(void) {
  rift_pools_init(BUMP_CAP, 32);
  oom_called = 0; oom_pool = NULL;
  rift_set_oom_handler(capture_oom);
  rift_longlived_alloc(128); /* exceeds cap */
  EXPECT(oom_called == 1, "longlived OOM handler not invoked");
  EXPECT(oom_pool != NULL && strcmp(oom_pool, "longlived") == 0, "wrong pool name");
  rift_pools_deinit();
  rift_set_oom_handler(NULL);
}

/* ---- Diagnostics ---- */

static void diagnostics_track_usage(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  EXPECT(rift_bump_used() == 0, "bump used should start at 0");
  EXPECT(rift_longlived_used() == 0, "longlived used should start at 0");
  rift_bump_alloc(64);
  EXPECT(rift_bump_used() == 64, "bump used should be 64");
  void *block = rift_longlived_alloc(64);
  EXPECT(rift_longlived_used() >= sizeof(rift_block_header) + 64,
         "longlived used incorrect");
  EXPECT(rift_longlived_peak_used() == rift_longlived_used(),
         "longlived peak used incorrect");
  rift_longlived_free(block);
  EXPECT(rift_longlived_peak_used() >= sizeof(rift_block_header) + 64,
         "longlived peak should survive reclamation");
  rift_pools_deinit();
}

static void free_bytes_reflects_available_classes(void) {
  rift_pools_init(BUMP_CAP, LL_CAP);
  size_t before = rift_longlived_free_bytes();
  void *p = rift_longlived_alloc(64);
  EXPECT(rift_longlived_free_bytes() < before, "allocation did not consume free capacity");
  rift_longlived_free(p);
  EXPECT(rift_longlived_free_bytes() > 0, "free bytes should reflect recovered capacity");
  rift_pools_deinit();
}

int main(void) {
  printf("Rift pool runtime tests\n\n");

  RUN(bump_alloc_returns_nonnull);
  RUN(bump_alloc_advances_top);
  RUN(bump_save_restore);
  RUN(bump_save_restore_nested);
  RUN(bump_alignment);

  RUN(longlived_alloc_returns_payload);
  RUN(longlived_size_rounds_to_alignment);
  RUN(longlived_free_marks_free);
  RUN(longlived_free_then_realloc_reuses);
  RUN(longlived_static_sentinel_not_freed);
  RUN(longlived_payload_writable);
  RUN(longlived_payload_alignment);

  RUN(reclaim_drains_magazines_and_coalesces_buddies);
  RUN(reclaim_does_not_merge_live_with_free);
  RUN(reclaim_chains_three_adjacent);
  RUN(coalesced_region_serves_smaller_allocation);
  RUN(common_short_lived_workload_has_constant_scan_depth);
  RUN(hot_small_block_cache_avoids_a_list_scan);
  RUN(collect_reclaims_magazines_for_a_larger_class);
  RUN(fragmented_large_request_uses_bounded_buddy_classes);

  RUN(bump_oom_invokes_handler);
  RUN(longlived_oom_invokes_handler);

  RUN(diagnostics_track_usage);
  RUN(free_bytes_reflects_available_classes);

  printf("\n%d/%d passed (%d failed)\n", tests_passed, tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
