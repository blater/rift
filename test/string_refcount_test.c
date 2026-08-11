/*****************************************************
 * Tests for __string_retain / __string_release.
 * Phase E.a — verifies the three-class discriminant against synthesised
 * backings since no Rock-language path populates `backing` yet.
 *****************************************************/

#include "../src/lib/pools.h"
#include "../src/lib/typedefs.h"
#include "../src/lib/fundefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUMP_CAP   (1u * 1024u * 1024u)
#define LL_CAP     (1u * 1024u * 1024u)

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define EXPECT(cond, msg)                                                    \
  do {                                                                        \
    if (!(cond)) {                                                            \
      fprintf(stderr, "  FAIL: %s (%s)\n", msg, #cond);                       \
      tests_failed++;                                                         \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define RUN(fn)                                                              \
  do {                                                                        \
    tests_run++;                                                              \
    int before = tests_failed;                                                \
    fn();                                                                     \
    if (tests_failed == before) {                                             \
      tests_passed++;                                                         \
      printf("  ok %s\n", #fn);                                               \
    } else {                                                                  \
      printf("  FAIL %s\n", #fn);                                             \
    }                                                                         \
  } while (0)

#define IS_FREE_STATE(refcount) \
  ((refcount) == ROCK_RC_FREE || (refcount) == ROCK_RC_MAGAZINE)

/* ---- Helpers to synthesise a longlived-backed string descriptor ---- */

/* Allocate a longlived block of `payload_size` bytes, copy `data` into it,
 * and return a string descriptor pointing at it with refcount = 1. */
static string make_longlived_string(const char *data, size_t length) {
  char *payload = (char *)rock_longlived_alloc(length + 1);
  memcpy(payload, data, length);
  payload[length] = 0;
  string s;
  s.data     = payload;
  s.length   = length;
  s.capacity = length;
  s.backing  = ((rock_block_header *)payload) - 1;
  return s;
}

/* ---- Tests ---- */

static void retain_release_on_null_backing_is_noop(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s;
  s.data     = "hello";  /* static literal-ish */
  s.length   = 5;
  s.capacity = 0;
  s.backing  = NULL;

  /* Should be safe to call any number of times; no refcount, no free. */
  __string_retain(s);
  __string_retain(s);
  __string_release(s);
  __string_release(s);
  __string_release(s);  /* extra releases also safe with NULL backing */

  rock_pools_deinit();
}

static void retain_release_on_static_sentinel_is_noop(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);

  /* Synthesise a static-sentinel block: header in static storage, refcount = 0xFFFF. */
  static char static_block[sizeof(rock_block_header) + 8];
  rock_block_header *h = (rock_block_header *)static_block;
  h->size = 8;
  h->refcount = ROCK_RC_STATIC;
  char *payload = static_block + sizeof(rock_block_header);
  memcpy(payload, "static!\0", 8);

  string s;
  s.data     = payload;
  s.length   = 7;
  s.capacity = 0;
  s.backing  = h;

  /* retain/release must NOT touch the refcount of a static block. */
  __string_retain(s);
  __string_retain(s);
  EXPECT(h->refcount == ROCK_RC_STATIC, "static refcount changed by retain");
  __string_release(s);
  __string_release(s);
  EXPECT(h->refcount == ROCK_RC_STATIC, "static refcount changed by release");

  rock_pools_deinit();
}

static void retain_increments_longlived_refcount(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s = make_longlived_string("hello", 5);
  EXPECT(s.backing->refcount == 1, "fresh longlived block starts at rc=1");
  __string_retain(s);
  EXPECT(s.backing->refcount == 2, "retain should increment to 2");
  __string_retain(s);
  EXPECT(s.backing->refcount == 3, "second retain should increment to 3");
  rock_pools_deinit();
}

static void release_decrements_longlived_refcount(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s = make_longlived_string("hello", 5);
  __string_retain(s);
  __string_retain(s);
  EXPECT(s.backing->refcount == 3, "after two retains rc should be 3");
  __string_release(s);
  EXPECT(s.backing->refcount == 2, "release should dec to 2");
  __string_release(s);
  EXPECT(s.backing->refcount == 1, "second release should dec to 1");
  /* Don't drop to zero in this test so we can inspect the block. */
  rock_pools_deinit();
}

static void release_to_zero_frees_block(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s = make_longlived_string("hello", 5);
  /* rc = 1 from make; release should drop to 0 and free. */
  __string_release(s);
  /* After free, the block's refcount is set to ROCK_RC_FREE by the
   * pool runtime's freelist push. */
  EXPECT(IS_FREE_STATE(s.backing->refcount),
         "freed block should be marked as free");
  rock_pools_deinit();
}

static void freed_block_can_be_reallocated(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s1 = make_longlived_string("first", 5);
  void *first_payload = s1.data;
  __string_release(s1);  /* freed */

  /* Subsequent same-size alloc should reuse the freed block. */
  string s2 = make_longlived_string("second", 6);
  EXPECT(s2.data == first_payload,
         "second alloc should reuse the freed block (same size class)");
  EXPECT(s2.backing->refcount == 1, "reallocated block must start at rc=1");
  rock_pools_deinit();
}

static void multiple_descriptors_share_backing(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string a = make_longlived_string("shared", 6);
  string b = a;  /* descriptor copy; same backing */
  __string_retain(b);  /* simulating an assignment that retains */
  EXPECT(a.backing->refcount == 2, "after retain rc should be 2");
  EXPECT(a.backing == b.backing, "both descriptors share backing");
  __string_release(b);
  EXPECT(a.backing->refcount == 1, "after release rc should be 1");
  __string_release(a);
  EXPECT(IS_FREE_STATE(a.backing->refcount),
         "second release should free the block");
  rock_pools_deinit();
}

/* ---- Phase F: __return_string ---- */

static void return_static_passes_through_unchanged(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  static char static_block[sizeof(rock_block_header) + 8];
  rock_block_header *h = (rock_block_header *)static_block;
  h->size = 8;
  h->refcount = ROCK_RC_STATIC;
  char *payload = static_block + sizeof(rock_block_header);
  memcpy(payload, "static!\0", 8);

  string s;
  s.data     = payload;
  s.length   = 7;
  s.capacity = 0;
  s.backing  = h;

  string r = __return_string(s);
  EXPECT(r.backing == h, "static return should preserve backing pointer");
  EXPECT(r.data == payload, "static return should preserve data pointer");
  EXPECT(h->refcount == ROCK_RC_STATIC, "static refcount must be unchanged");
  rock_pools_deinit();
}

static void return_longlived_increments_refcount(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  string s = make_longlived_string("hello", 5);
  EXPECT(s.backing->refcount == 1, "fresh block starts at rc=1");
  string r = __return_string(s);
  EXPECT(r.backing == s.backing, "longlived return must share backing");
  EXPECT(s.backing->refcount == 2, "longlived return must inc refcount");
  /* Two independent owners now; release one, the block survives. */
  __string_release(s);
  EXPECT(r.backing->refcount == 1, "after one release rc=1, block alive");
  __string_release(r);
  EXPECT(IS_FREE_STATE(r.backing->refcount), "second release frees");
  rock_pools_deinit();
}

static void return_bump_allocates_longlived_copy(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  /* Synthesise a bump-backed descriptor: backing == NULL, data points at
   * a buffer we allocate from the bump pool. */
  char *bump_payload = (char *)rock_bump_alloc(8);
  memcpy(bump_payload, "bump!", 5);

  string s;
  s.data     = bump_payload;
  s.length   = 5;
  s.capacity = 0;
  s.backing  = NULL;

  string r = __return_string(s);
  EXPECT(r.backing != NULL, "bump return must allocate fresh backing");
  EXPECT(r.backing->refcount == 1, "fresh longlived backing starts at rc=1");
  EXPECT(r.length == 5, "length preserved");
  EXPECT(r.data != bump_payload, "data must point at new longlived block");
  EXPECT(memcmp(r.data, "bump!", 5) == 0, "bytes copied");
  __string_release(r);
  EXPECT(IS_FREE_STATE(r.backing->refcount), "release frees the copy");
  rock_pools_deinit();
}

/* ---- Aggregate handle helpers (Phase F step 3) ---- */

static void handle_retain_release_on_null_is_noop(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  EXPECT(__handle_retain(NULL) == NULL, "retain(NULL) returns NULL");
  __handle_release(NULL);  /* must not crash */
  rock_pools_deinit();
}

static void handle_retain_increments_refcount(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *payload = rock_longlived_alloc(64);
  rock_block_header *h = ((rock_block_header *)payload) - 1;
  EXPECT(h->refcount == 1, "fresh handle rc=1");
  void *p2 = __handle_retain(payload);
  EXPECT(p2 == payload, "retain returns same pointer");
  EXPECT(h->refcount == 2, "retain bumps rc");
  __handle_release(payload);
  EXPECT(h->refcount == 1, "release dec rc");
  __handle_release(payload);
  EXPECT(IS_FREE_STATE(h->refcount), "second release frees");
  rock_pools_deinit();
}

static void return_handle_increments_refcount(void) {
  rock_pools_init(BUMP_CAP, LL_CAP);
  void *payload = rock_longlived_alloc(32);
  rock_block_header *h = ((rock_block_header *)payload) - 1;
  EXPECT(h->refcount == 1, "fresh rc=1");
  void *r = __return_handle(payload);
  EXPECT(r == payload, "return passes the handle through");
  EXPECT(h->refcount == 2, "return bumps rc for caller ownership");
  /* Simulate callee-side cleanup releasing its local. */
  __handle_release(payload);
  EXPECT(h->refcount == 1, "after callee release, caller still owns rc=1");
  /* Simulate caller scope exit. */
  __handle_release(r);
  EXPECT(IS_FREE_STATE(h->refcount), "caller's release frees");
  rock_pools_deinit();
}

int main(void) {
  printf("Phase E.a / F — string + aggregate refcount tests\n\n");

  RUN(retain_release_on_null_backing_is_noop);
  RUN(retain_release_on_static_sentinel_is_noop);
  RUN(retain_increments_longlived_refcount);
  RUN(release_decrements_longlived_refcount);
  RUN(release_to_zero_frees_block);
  RUN(freed_block_can_be_reallocated);
  RUN(multiple_descriptors_share_backing);

  RUN(return_static_passes_through_unchanged);
  RUN(return_longlived_increments_refcount);
  RUN(return_bump_allocates_longlived_copy);

  RUN(handle_retain_release_on_null_is_noop);
  RUN(handle_retain_increments_refcount);
  RUN(return_handle_increments_refcount);

  printf("\n%d/%d passed (%d failed)\n", tests_passed, tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
