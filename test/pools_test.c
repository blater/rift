/*****************************************************
 * Tests for the Rock pool runtime (src/lib/pools.{h,c}).
 * Standalone C harness — no Rock-language integration yet.
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
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p = rock_bump_alloc(64);
  EXPECT(p != NULL, "bump_alloc returned NULL");
  rock_pools_deinit();
}

static void bump_alloc_advances_top(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  size_t before = rock_bump_used();
  rock_bump_alloc(64);
  EXPECT(rock_bump_used() == before + 64, "bump top did not advance by 64");
  rock_pools_deinit();
}

static void bump_save_restore(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  rock_bump_alloc(32);
  rock_bump_mark mark = rock_bump_save();
  rock_bump_alloc(128);
  rock_bump_alloc(64);
  EXPECT(rock_bump_used() == 32 + 128 + 64, "bump top wrong before restore");
  rock_bump_restore(mark);
  EXPECT(rock_bump_used() == 32, "bump top did not restore to mark");
  rock_pools_deinit();
}

static void bump_save_restore_nested(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  rock_bump_mark m0 = rock_bump_save();
  rock_bump_alloc(16);
  rock_bump_mark m1 = rock_bump_save();
  rock_bump_alloc(32);
  rock_bump_mark m2 = rock_bump_save();
  rock_bump_alloc(64);
  rock_bump_restore(m2);
  EXPECT(rock_bump_used() == 16 + 32, "inner restore wrong");
  rock_bump_restore(m1);
  EXPECT(rock_bump_used() == 16, "middle restore wrong");
  rock_bump_restore(m0);
  EXPECT(rock_bump_used() == 0, "outer restore wrong");
  rock_pools_deinit();
}

static void bump_alignment(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  /* Allocate odd sizes; every result must meet the runtime alignment. */
  void *a = rock_bump_alloc(1);
  void *b = rock_bump_alloc(1);
  void *c = rock_bump_alloc(1);
  EXPECT(((uintptr_t)a % sizeof(void *)) == 0, "first bump allocation is misaligned");
  EXPECT(((uintptr_t)b % sizeof(void *)) == 0, "second bump allocation is misaligned");
  EXPECT(((uintptr_t)c % sizeof(void *)) == 0, "third bump allocation is misaligned");
  rock_pools_deinit();
}

/* ---- Longlived pool ---- */

static void longlived_alloc_returns_payload(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p = rock_longlived_alloc(16);
  EXPECT(p != NULL, "longlived_alloc returned NULL");
  rock_block_header *h = ((rock_block_header *)p) - 1;
  EXPECT(h->refcount == 1, "fresh block refcount must be 1");
  EXPECT(h->size >= 16, "block size below requested");
  rock_pools_deinit();
}

static void longlived_size_rounds_to_alignment(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p = rock_longlived_alloc(20);
  rock_block_header *h = ((rock_block_header *)p) - 1;
  EXPECT(h->size >= 20, "size is below request");
  EXPECT((h->size % sizeof(void *)) == 0, "size is not allocation-aligned");
  rock_pools_deinit();
}

static void longlived_free_marks_free(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p = rock_longlived_alloc(16);
  rock_longlived_free(p);
  rock_block_header *h = ((rock_block_header *)p) - 1;
  EXPECT(h->refcount == ROCK_RC_MAGAZINE || h->refcount == ROCK_RC_FREE,
         "freed block must enter a magazine or buddy free list");
  rock_pools_deinit();
}

static void longlived_free_then_realloc_reuses(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rock_longlived_alloc(16);
  rock_longlived_free(p1);
  void *p2 = rock_longlived_alloc(16);
  EXPECT(p1 == p2, "freed block was not reused on subsequent same-size alloc");
  rock_pools_deinit();
}

static void longlived_static_sentinel_not_freed(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p = rock_longlived_alloc(16);
  rock_block_header *h = ((rock_block_header *)p) - 1;
  h->refcount = ROCK_RC_STATIC;
  rock_longlived_free(p); /* should be a no-op */
  EXPECT(h->refcount == ROCK_RC_STATIC, "static block was modified by free");
  rock_pools_deinit();
}

static void longlived_payload_writable(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  char *p = (char *)rock_longlived_alloc(64);
  memset(p, 0xAB, 64);
  for (int i = 0; i < 64; i++) {
    EXPECT((unsigned char)p[i] == 0xAB, "payload byte not writable");
  }
  rock_pools_deinit();
}

static void longlived_payload_alignment(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *a = rock_longlived_alloc(1);
  void *b = rock_longlived_alloc(17);
  void *c = rock_longlived_alloc(63);
  EXPECT(((uintptr_t)a % sizeof(void *)) == 0, "first longlived payload is misaligned");
  EXPECT(((uintptr_t)b % sizeof(void *)) == 0, "second longlived payload is misaligned");
  EXPECT(((uintptr_t)c % sizeof(void *)) == 0, "third longlived payload is misaligned");
  rock_pools_deinit();
}

/* ---- Reclaim ---- */

static void reclaim_drains_magazines_and_coalesces_buddies(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rock_longlived_alloc(16);
  void *p2 = rock_longlived_alloc(16);
  rock_longlived_free(p1);
  rock_longlived_free(p2);
  rock_longlived_reclaim();
  EXPECT(rock_longlived_largest_free_block() >= 32,
         "collection did not make a larger buddy class available");
  rock_pools_deinit();
}

static void reclaim_does_not_merge_live_with_free(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rock_longlived_alloc(16);
  void *p2 = rock_longlived_alloc(16);
  void *p3 = rock_longlived_alloc(16);
  rock_longlived_free(p1);
  rock_longlived_free(p3);
  /* p2 is live between two free blocks; reclaim must not merge across it. */
  rock_longlived_reclaim();
  rock_block_header *h1 = ((rock_block_header *)p1) - 1;
  rock_block_header *h2 = ((rock_block_header *)p2) - 1;
  rock_block_header *h3 = ((rock_block_header *)p3) - 1;
  EXPECT(h2->refcount == 1, "live block was disturbed by reclaim");
  EXPECT(h1->refcount == ROCK_RC_FREE || h1->refcount == ROCK_RC_MAGAZINE,
         "first free block was disturbed");
  EXPECT(h3->refcount == ROCK_RC_FREE || h3->refcount == ROCK_RC_MAGAZINE,
         "last free block was disturbed");
  rock_pools_deinit();
}

static void reclaim_chains_three_adjacent(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *p1 = rock_longlived_alloc(16);
  void *p2 = rock_longlived_alloc(16);
  void *p3 = rock_longlived_alloc(16);
  rock_longlived_free(p1);
  rock_longlived_free(p2);
  rock_longlived_free(p3);
  rock_longlived_reclaim();
  EXPECT(rock_longlived_largest_free_block() >= 32,
         "three frees did not restore a useful buddy class");
  rock_pools_deinit();
}

static void coalesced_region_serves_smaller_allocation(void) {
  rock_pools_init(BUMP_CAP, 256);
  void *p1 = rock_longlived_alloc(16);
  void *p2 = rock_longlived_alloc(16);
  void *p3 = rock_longlived_alloc(16);
  rock_longlived_free(p1);
  rock_longlived_free(p2);
  rock_longlived_free(p3);
  EXPECT(rock_longlived_largest_free_block() >= 48,
         "three freed blocks were not coalesced into a reusable region");
  void *reuse = rock_longlived_alloc(32);
  EXPECT(reuse != NULL, "smaller allocation did not use recovered capacity");
  rock_pools_deinit();
}

static void common_short_lived_workload_has_constant_scan_depth(void) {
  rock_pools_init(1024, 6144);
  rock_allocator_stats_reset();
  for (int i = 0; i < 50; i++) {
    void *number = rock_longlived_alloc(8);
    void *message = rock_longlived_alloc(16);
    rock_longlived_free(message);
    rock_longlived_free(number);
  }
  rock_allocator_stats stats = rock_allocator_stats_get();
  EXPECT(stats.allocations == 100, "short-lived workload allocation count is wrong");
  EXPECT(stats.allocation_scan_steps == 0 && stats.free_scan_steps == 0,
         "normal workload must not scan heap blocks");
  EXPECT(stats.magazine_hits >= 90,
         "short-lived workload did not remain on the magazine fast path");
  rock_pools_deinit();
}

static void hot_small_block_cache_avoids_a_list_scan(void) {
  rock_pools_init(1024, 1024);
  void *before = rock_longlived_alloc(16);
  void *hot = rock_longlived_alloc(8);
  void *after = rock_longlived_alloc(16);
  rock_longlived_free(hot);
  rock_allocator_stats_reset();
  void *reuse = rock_longlived_alloc(8);
  rock_allocator_stats stats = rock_allocator_stats_get();
  EXPECT(reuse == hot, "hot cache did not reuse the recently freed small block");
  EXPECT(stats.allocation_scan_steps == 0,
         "hot-cache allocation should not traverse the free list");
  rock_longlived_free(before);
  rock_longlived_free(reuse);
  rock_longlived_free(after);
  rock_pools_deinit();
}

static void collect_reclaims_magazines_for_a_larger_class(void) {
  rock_pools_init(1024, 128);
  void *blocks[4];
  for (int i = 0; i < 4; i++) blocks[i] = rock_longlived_alloc(1);
  for (int i = 0; i < 4; i++) rock_longlived_free(blocks[i]);
  EXPECT(rock_longlived_largest_free_block() < 16,
         "magazine-only capacity must not look like a larger class");
  rock_allocator_stats_reset();
  rock_collect();
  rock_allocator_stats stats = rock_allocator_stats_get();
  EXPECT(stats.collect_calls == 1, "explicit collection was not counted");
  EXPECT(rock_longlived_largest_free_block() >= 16,
         "collection did not coalesce cached blocks into a larger class");
  EXPECT(rock_longlived_alloc(16) != NULL,
         "larger allocation did not use collected capacity");
  rock_pools_deinit();
}

static void fragmented_large_request_uses_bounded_buddy_classes(void) {
  rock_pools_init(1024, 1024);
  void *blocks[16];
  for (int i = 0; i < 16; i++) blocks[i] = rock_longlived_alloc(16);
  for (int i = 0; i < 16; i += 2) rock_longlived_free(blocks[i]);
  rock_allocator_stats_reset();
  void *large = rock_longlived_alloc(24);
  rock_allocator_stats stats = rock_allocator_stats_get();
  EXPECT(large != NULL, "fragmented fallback allocation failed unexpectedly");
  EXPECT(stats.allocation_scan_steps == 0 && stats.free_scan_steps == 0,
         "fragmented allocation must not inspect heap blocks");
  rock_longlived_free(large);
  for (int i = 1; i < 16; i += 2) rock_longlived_free(blocks[i]);
  rock_pools_deinit();
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
  rock_pools_init(64, LL_CAP);
  oom_called = 0; oom_pool = NULL;
  rock_set_oom_handler(capture_oom);
  rock_bump_alloc(128); /* exceeds cap */
  EXPECT(oom_called == 1, "bump OOM handler not invoked");
  EXPECT(oom_pool != NULL && strcmp(oom_pool, "bump") == 0, "wrong pool name");
  rock_pools_deinit();
  rock_set_oom_handler(NULL);
}

static void longlived_oom_invokes_handler(void) {
  rock_pools_init(BUMP_CAP, 32);
  oom_called = 0; oom_pool = NULL;
  rock_set_oom_handler(capture_oom);
  rock_longlived_alloc(128); /* exceeds cap */
  EXPECT(oom_called == 1, "longlived OOM handler not invoked");
  EXPECT(oom_pool != NULL && strcmp(oom_pool, "longlived") == 0, "wrong pool name");
  rock_pools_deinit();
  rock_set_oom_handler(NULL);
}

/* ---- Diagnostics ---- */

static void diagnostics_track_usage(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  EXPECT(rock_bump_used() == 0, "bump used should start at 0");
  EXPECT(rock_longlived_used() == 0, "longlived used should start at 0");
  rock_bump_alloc(64);
  EXPECT(rock_bump_used() == 64, "bump used should be 64");
  void *block = rock_longlived_alloc(64);
  EXPECT(rock_longlived_used() >= sizeof(rock_block_header) + 64,
         "longlived used incorrect");
  EXPECT(rock_longlived_peak_used() == rock_longlived_used(),
         "longlived peak used incorrect");
  rock_longlived_free(block);
  EXPECT(rock_longlived_peak_used() >= sizeof(rock_block_header) + 64,
         "longlived peak should survive reclamation");
  rock_pools_deinit();
}

static void free_bytes_reflects_available_classes(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  size_t before = rock_longlived_free_bytes();
  void *p = rock_longlived_alloc(64);
  EXPECT(rock_longlived_free_bytes() < before, "allocation did not consume free capacity");
  rock_longlived_free(p);
  EXPECT(rock_longlived_free_bytes() > 0, "free bytes should reflect recovered capacity");
  rock_pools_deinit();
}

int main(void) {
  printf("Rock pool runtime tests\n\n");

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
