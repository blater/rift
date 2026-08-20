/*****************************************************
 * Tests for __string_retain / __string_release.
 * Phase E.a — verifies the three-class discriminant against synthesised
 * backings since no Rift-language path populates `backing` yet.
 *****************************************************/

#include "../src/lib/pools.h"
#include "../src/lib/typedefs.h"
#include "../src/lib/fundefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUMP_CAP   (1u * 1024u * 1024u)
#define LL_CAP     (1u * 1024u * 1024u)

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void init_test_arena(size_t total_capacity) {
  rift_arena_options options = {
      .memory_max = total_capacity,
      .memory_max_present = 1,
  };
  rift_pools_init(&options);
}

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

#define IS_FREE_STATE(refcount) ((refcount) == RIFT_RC_FREE)

/* ---- Helpers to synthesise a longlived-backed string descriptor ---- */

/* Allocate a longlived block of `payload_size` bytes, copy `data` into it,
 * and return a string descriptor pointing at it with refcount = 1. */
static string make_longlived_string(const char *data, size_t length) {
  char *payload = (char *)rift_longlived_alloc(length + 1);
  memcpy(payload, data, length);
  payload[length] = 0;
  string s;
  s.data     = payload;
  s.length   = length;
  s.capacity = length;
  s.backing  = ((rift_block_header *)payload) - 1;
  return s;
}

static string make_produced_string(const char *data, size_t length) {
  string s;
  __rift_make_longlived_string(&s, length);
  memcpy(s.data, data, length);
  s.data[length] = 0;
  return s;
}

/* ---- Tests ---- */

static void retain_release_on_null_backing_is_noop(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
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

  rift_pools_deinit();
}

static void retain_release_on_static_sentinel_is_noop(void) {
  init_test_arena(BUMP_CAP + LL_CAP);

  /* Synthesise a static-sentinel block: header in static storage, refcount = 0xFFFF. */
  static char static_block[sizeof(rift_block_header) + 8];
  rift_block_header *h = (rift_block_header *)static_block;
  h->size = 8;
  h->refcount = RIFT_RC_STATIC;
  char *payload = static_block + sizeof(rift_block_header);
  memcpy(payload, "static!\0", 8);

  string s;
  s.data     = payload;
  s.length   = 7;
  s.capacity = 0;
  s.backing  = h;

  /* retain/release must NOT touch the refcount of a static block. */
  __string_retain(s);
  __string_retain(s);
  EXPECT(h->refcount == RIFT_RC_STATIC, "static refcount changed by retain");
  __string_release(s);
  __string_release(s);
  EXPECT(h->refcount == RIFT_RC_STATIC, "static refcount changed by release");

  rift_pools_deinit();
}

static void retain_increments_longlived_refcount(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s = make_longlived_string("hello", 5);
  EXPECT(s.backing->refcount == 1, "fresh longlived block starts at rc=1");
  __string_retain(s);
  EXPECT(s.backing->refcount == 2, "retain should increment to 2");
  __string_retain(s);
  EXPECT(s.backing->refcount == 3, "second retain should increment to 3");
  rift_pools_deinit();
}

static void release_decrements_longlived_refcount(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s = make_longlived_string("hello", 5);
  __string_retain(s);
  __string_retain(s);
  EXPECT(s.backing->refcount == 3, "after two retains rc should be 3");
  __string_release(s);
  EXPECT(s.backing->refcount == 2, "release should dec to 2");
  __string_release(s);
  EXPECT(s.backing->refcount == 1, "second release should dec to 1");
  /* Don't drop to zero in this test so we can inspect the block. */
  rift_pools_deinit();
}

static void release_to_zero_frees_block(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s = make_longlived_string("hello", 5);
  /* rc = 1 from make; release should drop to 0 and free. */
  __string_release(s);
  /* After free, the block's refcount is set to RIFT_RC_FREE by the
   * pool runtime's freelist push. */
  EXPECT(IS_FREE_STATE(s.backing->refcount),
         "freed block should be marked as free");
  rift_pools_deinit();
}

static void freed_block_can_be_reallocated(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s1 = make_longlived_string("first", 5);
  void *first_payload = s1.data;
  __string_release(s1);  /* freed */

  /* Subsequent same-size alloc should reuse the freed block. */
  string s2 = make_longlived_string("second", 6);
  EXPECT(s2.data == first_payload,
         "second alloc should reuse the freed block (same size class)");
  EXPECT(s2.backing->refcount == 1, "reallocated block must start at rc=1");
  rift_pools_deinit();
}

static void multiple_descriptors_share_backing(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
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
  rift_pools_deinit();
}

static void produced_capacity_uses_allocator_payload(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s = make_produced_string("ab", 2);
  EXPECT(s.capacity == (size_t)s.backing->size - 1,
         "produced capacity must expose the complete allocator payload");
  EXPECT(s.capacity >= s.length,
         "produced capacity must cover the visible string");
  __string_release(s);
  rift_pools_deinit();
}

static void append_reuses_unique_capacity(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string target = make_produced_string("ab", 2);
  string base = target;
  char *original_data = target.data;
  __string_retain(base);
  rift_allocator_stats_reset();
  __concat_append_owned(&base, "c", 1, 2);
  EXPECT(rift_allocator_stats_get().allocations == 0,
         "unique assignment backing with spare capacity must be reused");
  EXPECT(base.data == original_data && base.length == 3,
         "in-place append must preserve backing and update length");
  EXPECT(memcmp(base.data, "abc", 3) == 0,
         "in-place append produced incorrect bytes");
  __string_release(target);
  target = base;
  __string_release(target);
  rift_pools_deinit();
}

static void append_copy_on_write_preserves_alias(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string target = make_produced_string("ab", 2);
  string alias = target;
  string base = target;
  char *old_data = target.data;
  __string_retain(alias);
  __string_retain(base);
  __concat_append_owned(&base, "c", 1, 2);
  EXPECT(base.data != old_data,
         "an external alias must force copy-on-write");
  EXPECT(alias.length == 2 && memcmp(alias.data, "ab", 2) == 0,
         "copy-on-write must preserve aliased bytes");
  EXPECT(base.length == 3 && memcmp(base.data, "abc", 3) == 0,
         "copy-on-write result has incorrect bytes");
  __string_release(target);
  __string_release(alias);
  __string_release(base);
  rift_pools_deinit();
}

static void self_append_is_alias_safe(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string target = make_produced_string("xy", 2);
  string base = target;
  string suffix = target;
  __string_retain(base);
  __string_retain(suffix);
  __concat_append_owned(&base, suffix.data, suffix.length, 2);
  EXPECT(base.length == 4 && memcmp(base.data, "xyxy", 4) == 0,
         "self append must preserve the source while growing");
  __string_release(suffix);
  __string_release(target);
  target = base;
  __string_release(target);
  rift_pools_deinit();
}

static void repeated_append_allocations_are_logarithmic(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  string s = make_produced_string("", 0);
  rift_allocator_stats_reset();
  for (int i = 0; i < 1000; i++)
    __concat_append_owned(&s, ".", 1, 1);
  EXPECT(s.length == 1000, "repeated append produced the wrong length");
  EXPECT(rift_allocator_stats_get().allocations <= 9,
         "repeated append must double explicit string capacity");
  for (size_t i = 0; i < s.length; i++)
    EXPECT(s.data[i] == '.', "repeated append corrupted content");
  __string_release(s);
  rift_pools_deinit();
}

static jmp_buf append_oom_jump;

static void jump_on_oom(const char *pool_name, size_t requested,
                        size_t available) {
  (void)pool_name;
  (void)requested;
  (void)available;
  longjmp(append_oom_jump, 1);
}

static void append_oom_leaves_source_unchanged(void) {
  init_test_arena(112);
  string s = make_produced_string("ab", 2);
  char *original_data = s.data;
  rift_block_header *original_backing = s.backing;
  rift_set_oom_handler(jump_on_oom);
  if (setjmp(append_oom_jump) == 0) {
    __concat_append_owned(&s, "0123456789012345678901234567890123456789",
                          40, 1);
    EXPECT(0, "append should report OOM when replacement cannot be allocated");
  }
  rift_set_oom_handler(NULL);
  EXPECT(s.data == original_data && s.backing == original_backing &&
             s.length == 2 && memcmp(s.data, "ab", 2) == 0,
         "failed growth must leave the source descriptor and bytes unchanged");
  __string_release(s);
  rift_pools_deinit();
}

static void append_length_overflow_halts(void) {
  fflush(NULL);
  pid_t child = fork();
  int status = 0;
  EXPECT(child >= 0, "fork failed");
  if (child == 0) {
    string s;
    init_test_arena(BUMP_CAP + LL_CAP);
    s = make_produced_string("ab", 2);
    __concat_append_owned(&s, "x", SIZE_MAX, 1);
    _exit(0);
  }
  EXPECT(waitpid(child, &status, 0) == child, "waitpid failed");
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) != 0,
         "append length overflow must halt before pointer arithmetic");
}

static void observe_oom_and_return(const char *pool_name, size_t requested,
                                   size_t available) {
  (void)pool_name;
  (void)requested;
  (void)available;
}

static void returning_oom_handler_still_halts_string_construction(void) {
  fflush(NULL);
  pid_t child = fork();
  int status = 0;
  EXPECT(child >= 0, "fork failed");
  if (child == 0) {
    string s;
    init_test_arena(128);
    rift_set_oom_handler(observe_oom_and_return);
    __rift_make_longlived_string(&s, 1000);
    _exit(0);
  }
  EXPECT(waitpid(child, &status, 0) == child, "waitpid failed");
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) != 0,
         "a returning OOM observer must not lead to a NULL backing dereference");
}

static void impossible_string_length_halts_before_wraparound(void) {
  fflush(NULL);
  pid_t child = fork();
  int status = 0;
  EXPECT(child >= 0, "fork failed");
  if (child == 0) {
    string s;
    init_test_arena(BUMP_CAP + LL_CAP);
    __rift_make_longlived_string(&s, SIZE_MAX);
    _exit(0);
  }
  EXPECT(waitpid(child, &status, 0) == child, "waitpid failed");
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) != 0,
         "SIZE_MAX string length must halt instead of wrapping length + 1");
}

/* ---- Phase F: __return_string ---- */

static void return_static_passes_through_unchanged(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  static char static_block[sizeof(rift_block_header) + 8];
  rift_block_header *h = (rift_block_header *)static_block;
  h->size = 8;
  h->refcount = RIFT_RC_STATIC;
  char *payload = static_block + sizeof(rift_block_header);
  memcpy(payload, "static!\0", 8);

  string s;
  s.data     = payload;
  s.length   = 7;
  s.capacity = 0;
  s.backing  = h;

  string r = __return_string(s);
  EXPECT(r.backing == h, "static return should preserve backing pointer");
  EXPECT(r.data == payload, "static return should preserve data pointer");
  EXPECT(h->refcount == RIFT_RC_STATIC, "static refcount must be unchanged");
  rift_pools_deinit();
}

static void return_longlived_increments_refcount(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
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
  rift_pools_deinit();
}

static void return_bump_allocates_longlived_copy(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  /* Synthesise a bump-backed descriptor: backing == NULL, data points at
   * a buffer we allocate from the bump pool. */
  char *bump_payload = (char *)rift_bump_alloc(8);
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
  rift_pools_deinit();
}

/* ---- Aggregate handle helpers (Phase F step 3) ---- */

static void handle_retain_release_on_null_is_noop(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  EXPECT(__handle_retain(NULL) == NULL, "retain(NULL) returns NULL");
  __handle_release(NULL);  /* must not crash */
  rift_pools_deinit();
}

static void handle_retain_increments_refcount(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  void *payload = rift_longlived_alloc(64);
  rift_block_header *h = ((rift_block_header *)payload) - 1;
  EXPECT(h->refcount == 1, "fresh handle rc=1");
  void *p2 = __handle_retain(payload);
  EXPECT(p2 == payload, "retain returns same pointer");
  EXPECT(h->refcount == 2, "retain bumps rc");
  __handle_release(payload);
  EXPECT(h->refcount == 1, "release dec rc");
  __handle_release(payload);
  EXPECT(IS_FREE_STATE(h->refcount), "second release frees");
  rift_pools_deinit();
}

static void return_handle_increments_refcount(void) {
  init_test_arena(BUMP_CAP + LL_CAP);
  void *payload = rift_longlived_alloc(32);
  rift_block_header *h = ((rift_block_header *)payload) - 1;
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
  rift_pools_deinit();
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
  RUN(produced_capacity_uses_allocator_payload);
  RUN(append_reuses_unique_capacity);
  RUN(append_copy_on_write_preserves_alias);
  RUN(self_append_is_alias_safe);
  RUN(repeated_append_allocations_are_logarithmic);
  RUN(append_oom_leaves_source_unchanged);
  RUN(append_length_overflow_halts);
  RUN(returning_oom_handler_still_halts_string_construction);
  RUN(impossible_string_length_halts_before_wraparound);

  RUN(return_static_passes_through_unchanged);
  RUN(return_longlived_increments_refcount);
  RUN(return_bump_allocates_longlived_copy);

  RUN(handle_retain_release_on_null_is_noop);
  RUN(handle_retain_increments_refcount);
  RUN(return_handle_increments_refcount);

  printf("\n%d/%d passed (%d failed)\n", tests_passed, tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
