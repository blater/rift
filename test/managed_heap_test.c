#include "../src/lib/managed_heap.h"
#include "../src/lib/pools.h"
#include "../src/lib/segregated_heap.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_ARENA_BYTES 131072u
#define TYPE_TRIVIAL 1u
#define TYPE_PARENT 2u

typedef struct parent_payload {
  managed_ref child;
  uint16_t marker;
} parent_payload;

_Alignas(16) static unsigned char test_arena[TEST_ARENA_BYTES];

const managed_static_entry rift_managed_static_table[] = {{"static", 6}};
const size_t rift_managed_static_count = 1;

static int tests_run;
static int tests_failed;
static int destruct_count;
static managed_type_id last_destroyed_type;
static jmp_buf fault_jump;
static int expect_fault;
static managed_fault_reason observed_fault;

#define CHECK(condition, message)                                              \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__);    \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define RUN(test)                                                               \
  do {                                                                          \
    int before = tests_failed;                                                  \
    tests_run++;                                                                \
    test();                                                                     \
    if (tests_failed == before) printf("  ok %s\n", #test);                   \
  } while (0)

static void test_fault(managed_fault_reason reason, size_t requested) {
  (void)requested;
  observed_fault = reason;
  if (expect_fault) longjmp(fault_jump, 1);
  fprintf(stderr, "unexpected managed fault %d\n", (int)reason);
  tests_failed++;
  longjmp(fault_jump, 1);
}

static void test_destroy(managed_type_id type, void *payload) {
  destruct_count++;
  last_destroyed_type = type;
  if (type == TYPE_PARENT) {
    parent_payload *parent = (parent_payload *)payload;
    if (parent->child) managed_release_typed(parent->child, TYPE_TRIVIAL);
  }
}

static void init_runtime(void) {
  managed_heap_options options;
  memset(test_arena, 0xa5, sizeof(test_arena));
  if (!rift_heap_init(test_arena, sizeof(test_arena), 1)) {
    fprintf(stderr, "FAIL: heap init rejected managed test arena\n");
    tests_failed++;
    return;
  }
  options.destroy = test_destroy;
  options.fault = test_fault;
  managed_heap_init(&options);
  destruct_count = 0;
  last_destroyed_type = 0;
  observed_fault = 0;
  expect_fault = 0;
}

static void deinit_runtime(void) {
  managed_heap_deinit();
  rift_heap_deinit();
}

static void exact_host_layout_and_static_facade(void) {
  managed_access_token access;
  managed_pin_token pin;
  managed_ref ref = MANAGED_REF_STATIC_BIT;
  CHECK(sizeof(managed_ref) == 4, "host reference width changed");
  CHECK(sizeof(managed_static_entry) == 16,
        "host static entry width changed");
  CHECK(sizeof(managed_access_token) == 8,
        "host access token width changed");
  CHECK(sizeof(managed_pin_token) == 16, "host pin token width changed");
  CHECK(managed_retain(ref) == ref, "static retain changed identity");
  managed_access_begin(ref, &access);
  CHECK(managed_access_ptr(&access) == rift_managed_static_table[0].payload,
        "static access did not resolve table entry");
  managed_access_end(&access);
  managed_pin_typed(ref, TYPE_TRIVIAL, &pin);
  CHECK(managed_pin_ptr(&pin) == rift_managed_static_table[0].payload,
        "static pin did not resolve table entry");
  managed_unpin_typed(&pin);
  CHECK(pin.ref == 0 && pin.payload == NULL, "static unpin did not clear token");
  managed_release_typed(ref, TYPE_TRIVIAL);
}

static void stable_access_retain_and_typed_final_release(void) {
  managed_ref ref;
  managed_access_token access;
  uint32_t *value;
  init_runtime();
  ref = managed_alloc(sizeof(uint32_t));
  CHECK(ref != 0 && !managed_ref_is_static(ref),
        "dynamic allocation did not return a dynamic identity");
  managed_access_begin(ref, &access);
  value = (uint32_t *)managed_access_ptr(&access);
  CHECK(value != NULL, "dynamic access returned null");
  *value = 0x12345678u;
  managed_access_end(&access);
  CHECK(access.payload == NULL, "access end did not clear raw pointer");
  CHECK(managed_retain(ref) == ref && managed_heap_test_refcount(ref) == 2,
        "retain did not preserve identity and increment ownership");
  managed_release_typed(ref, TYPE_TRIVIAL);
  CHECK(managed_heap_test_refcount(ref) == 1 && destruct_count == 0,
        "non-final release destroyed the object");
  managed_access_begin(ref, &access);
  CHECK(*(uint32_t *)managed_access_ptr(&access) == 0x12345678u,
        "identity did not preserve payload contents");
  managed_access_end(&access);
  managed_release_typed(ref, 77u);
  CHECK(destruct_count == 1 && last_destroyed_type == 77u,
        "final release did not dispatch the supplied type exactly once");
  CHECK(managed_heap_live_count() == 0, "final release left a live identity");
  deinit_runtime();
}

static void pin_owns_the_last_reference(void) {
  managed_ref ref;
  managed_pin_token pin;
  init_runtime();
  ref = managed_alloc(12);
  managed_pin_typed(ref, 91u, &pin);
  CHECK(pin.ref == ref && pin.payload != NULL,
        "pin did not publish a scoped token");
  CHECK(managed_heap_test_refcount(ref) == 2 &&
            managed_heap_test_pin_count(ref) == 1 &&
            managed_heap_pin_count() == 1,
        "pin did not retain and mark exactly one barrier");
  managed_release_typed(ref, TYPE_TRIVIAL);
  CHECK(managed_heap_test_refcount(ref) == 1 && destruct_count == 0,
        "owner release ignored the pin's strong retain");
  memset(managed_pin_ptr(&pin), 0x3c, 12);
  managed_unpin_typed(&pin);
  CHECK(pin.ref == 0 && pin.payload == NULL,
        "unpin did not invalidate its raw pointer first");
  CHECK(managed_heap_pin_count() == 0 && managed_heap_live_count() == 0,
        "last-owner unpin did not destroy and remove the identity");
  CHECK(destruct_count == 1 && last_destroyed_type == 91u,
        "unpin did not use its captured destructor type");
  deinit_runtime();
}

static void recursive_typed_destruction(void) {
  managed_ref child;
  managed_ref parent;
  managed_access_token access;
  init_runtime();
  child = managed_alloc(3);
  parent = managed_alloc(sizeof(parent_payload));
  managed_access_begin(parent, &access);
  ((parent_payload *)managed_access_ptr(&access))->child = child;
  ((parent_payload *)managed_access_ptr(&access))->marker = 0xbeefu;
  managed_access_end(&access);
  managed_release_typed(parent, TYPE_PARENT);
  CHECK(destruct_count == 2, "recursive destructor did not release its child");
  CHECK(managed_heap_live_count() == 0,
        "recursive destructor left parent or child live");
  deinit_runtime();
}

static void adaptive_radix_grows_contracts_and_reuses_lowest_id(void) {
  managed_ref refs[96];
  managed_heap_test_stats stats;
  size_t index;
  init_runtime();
  for (index = 0; index < 96; index++) {
    refs[index] = managed_alloc(2);
    CHECK(refs[index] == (managed_ref)(index + 1u),
          "dense allocation did not choose the lowest free identity");
  }
  stats = managed_heap_test_stats_get();
  CHECK(stats.tree_height >= 2 && stats.tree_height <= 4,
        "packed directory height exceeded its target bound");
  CHECK(stats.node_bytes > 256, "directory did not grow across radix pages");
  managed_release_typed(refs[22], TYPE_TRIVIAL);
  refs[22] = managed_alloc(2);
  CHECK(refs[22] == 23u, "released identity was not reused immediately");
  for (index = 0; index < 96; index += 2)
    managed_release_typed(refs[index], TYPE_TRIVIAL);
  for (index = 1; index < 96; index += 2)
    managed_release_typed(refs[index], TYPE_TRIVIAL);
  CHECK(managed_heap_live_count() == 0,
        "radix release did not contract the directory to zero");
  CHECK(managed_heap_test_stats_get().tree_height == 0,
        "empty directory retained a historical root");
  deinit_runtime();
}

static void allocation_failure_rolls_back_before_publish(void) {
  size_t before;
  init_runtime();
  before = managed_heap_test_digest();
  expect_fault = 1;
  managed_heap_test_fail_after(1); /* root leaf succeeds, payload fails */
  if (setjmp(fault_jump) == 0) {
    (void)managed_alloc(8);
    CHECK(0, "injected allocation fault returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_CAPACITY,
        "rollback fault had the wrong classification");
  managed_heap_test_fail_after(-1);
  CHECK(managed_heap_live_count() == 0,
        "failed allocation published a live identity");
  CHECK(managed_heap_test_digest() == before,
        "failed root transaction changed the heap/directory digest");
  {
    managed_ref refs[16];
    size_t index;
    for (index = 0; index < 16; index++) refs[index] = managed_alloc(1);
    before = managed_heap_test_digest();
    observed_fault = 0;
    expect_fault = 1;
    managed_heap_test_fail_after(2); /* new root+leaf succeed, payload fails */
    if (setjmp(fault_jump) == 0) {
      (void)managed_alloc(1);
      CHECK(0, "injected radix growth fault returned normally");
    }
    expect_fault = 0;
    managed_heap_test_fail_after(-1);
    CHECK(observed_fault == MANAGED_FAULT_CAPACITY,
          "radix growth rollback fault had the wrong classification");
    CHECK(managed_heap_live_count() == 16,
          "failed radix growth transaction changed live identities");
    CHECK(managed_heap_test_digest() == before,
          "failed radix growth transaction changed heap/directory state");
    for (index = 0; index < 16; index++)
      managed_release_typed(refs[index], TYPE_TRIVIAL);
  }
  deinit_runtime();
}

static void cache_hit_is_observable_and_bounded(void) {
  managed_ref ref;
  managed_access_token token;
  managed_heap_test_stats before;
  managed_heap_test_stats after;
  init_runtime();
  ref = managed_alloc(4);
  before = managed_heap_test_stats_get();
  managed_access_begin(ref, &token);
  managed_access_end(&token);
  managed_access_begin(ref, &token);
  managed_access_end(&token);
  after = managed_heap_test_stats_get();
  CHECK(after.cache_hits >= before.cache_hits + 2u,
        "cached access did not use the direct-mapped fast path");
  CHECK(after.node_visits == before.node_visits,
        "cache hits unexpectedly visited B+tree nodes");
  managed_release_typed(ref, TYPE_TRIVIAL);
  deinit_runtime();
}

static void invalid_tokens_fault_without_partial_publication(void) {
  managed_ref invalid_static =
      (managed_ref)(MANAGED_REF_STATIC_BIT | rift_managed_static_count);
  managed_access_token access;
  managed_pin_token pin;
  managed_pin_token sentinel;
  managed_ref ref;
  init_runtime();

  access.payload = (void *)(uintptr_t)0x1234u;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_access_begin(invalid_static, &access);
    CHECK(0, "invalid static access returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_INVALID_REF &&
            access.payload == (void *)(uintptr_t)0x1234u,
        "invalid static access published or changed its token");

  sentinel.payload = (void *)(uintptr_t)0x5678u;
  sentinel.ref = 77u;
  sentinel.type = 88u;
  pin = sentinel;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_pin_typed(invalid_static, 9u, &pin);
    CHECK(0, "invalid static pin returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_INVALID_REF &&
            memcmp(&pin, &sentinel, sizeof(pin)) == 0,
        "invalid static pin partially published its token");

  pin = sentinel;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_pin_typed(57u, 9u, &pin);
    CHECK(0, "invalid dynamic pin returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_INVALID_REF &&
            memcmp(&pin, &sentinel, sizeof(pin)) == 0,
        "invalid dynamic pin partially published its token");

  ref = managed_alloc(1);
  managed_heap_test_set_counts(ref, 1u, 0xffu);
  pin = sentinel;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_pin_typed(ref, 9u, &pin);
    CHECK(0, "pin-count overflow returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_PIN_OVERFLOW &&
            memcmp(&pin, &sentinel, sizeof(pin)) == 0,
        "pin-count overflow partially published its token");

  managed_heap_test_set_counts(ref, RIFT_RC_MAX_LIVE, 0u);
  pin = sentinel;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_pin_typed(ref, 9u, &pin);
    CHECK(0, "refcount overflow pin returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_REFCOUNT_OVERFLOW &&
            memcmp(&pin, &sentinel, sizeof(pin)) == 0,
        "refcount overflow partially published its token");

  managed_heap_test_set_counts(ref, 1u, 0u);
  pin.payload = NULL;
  pin.ref = ref;
  pin.type = 9u;
  sentinel = pin;
  observed_fault = 0;
  expect_fault = 1;
  if (setjmp(fault_jump) == 0) {
    managed_unpin_typed(&pin);
    CHECK(0, "malformed active pin token returned normally");
  }
  expect_fault = 0;
  CHECK(observed_fault == MANAGED_FAULT_INVALID_REF &&
            memcmp(&pin, &sentinel, sizeof(pin)) == 0,
        "malformed active pin token was changed before faulting");
  managed_release_typed(ref, TYPE_TRIVIAL);
  deinit_runtime();
}

int main(void) {
  printf("managed heap tests\n");
  RUN(exact_host_layout_and_static_facade);
  RUN(stable_access_retain_and_typed_final_release);
  RUN(pin_owns_the_last_reference);
  RUN(recursive_typed_destruction);
  RUN(adaptive_radix_grows_contracts_and_reuses_lowest_id);
  RUN(allocation_failure_rolls_back_before_publish);
  RUN(cache_hit_is_observable_and_bounded);
  RUN(invalid_tokens_fault_without_partial_publication);
  printf("%d tests, %d failures\n", tests_run, tests_failed);
  return tests_failed ? 1 : 0;
}
