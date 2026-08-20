#include "../src/lib/segregated_heap.h"
#include "../src/lib/pools.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_ARENA_BYTES 131072u

static union {
  max_align_t alignment;
  unsigned char bytes[TEST_ARENA_BYTES];
} test_arena;

static int tests_run;
static int tests_failed;

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
    int failures_before = tests_failed;                                         \
    tests_run++;                                                                \
    test();                                                                     \
    if (tests_failed == failures_before) printf("  ok %s\n", #test);          \
  } while (0)

static void init_heap_mode(size_t capacity, int permanent_limit) {
  memset(test_arena.bytes, 0xCD, sizeof(test_arena.bytes));
  if (!rift_heap_init(test_arena.bytes, capacity, permanent_limit)) {
    fprintf(stderr, "FAIL: heap init rejected test arena\n");
    tests_failed++;
  }
}

static void init_heap(size_t capacity) { init_heap_mode(capacity, 1); }

static void target_layout_and_exhaustive_bin_mapping(void) {
  unsigned int previous_request_index = 0;
  unsigned int previous_free_index = 0;
  size_t physical;
  CHECK(sizeof(rift_block_header) == 24,
        "LP64 public raw header layout changed");
  init_heap(4096);
  CHECK(rift_heap_block_overhead() == 32,
        "LP64 boundary prefix/header overhead is not exact");
  CHECK(rift_heap_minimum_physical_block() == 48,
        "LP64 minimum free block is not exact");
  for (physical = 48; physical <= 65520; physical += 16) {
    unsigned int request_first;
    unsigned int request_second;
    unsigned int free_first;
    unsigned int free_second;
    unsigned int request_index;
    unsigned int free_index;
    size_t charged;
    size_t satisfiable;
    CHECK(rift_heap_test_map_request(physical, &request_first,
                                     &request_second, &charged),
          "representable physical request did not map");
    CHECK(rift_heap_test_map_free(physical, &free_first, &free_second,
                                  &satisfiable),
          "representable free block did not map");
    CHECK(charged >= physical,
          "request mapping rounded below the required physical size");
    CHECK(satisfiable <= physical,
          "free mapping promised more than the block can satisfy");
    CHECK(charged - physical <= physical / 7u + 16u,
          "eight-way class rounding exceeded its documented bound");
    {
      unsigned int charged_free_first;
      unsigned int charged_free_second;
      size_t charged_satisfiable;
      CHECK(rift_heap_test_map_free(charged, &charged_free_first,
                                    &charged_free_second,
                                    &charged_satisfiable),
            "charged request size did not map as a free block");
      CHECK(charged_free_first == request_first &&
                charged_free_second == request_second &&
                charged_satisfiable == charged,
            "charged block did not return to its request class");
    }
    request_index = request_first * 8u + request_second;
    free_index = free_first * 8u + free_second;
    CHECK(physical == 48 || request_index >= previous_request_index,
          "request mapping is not monotonic");
    CHECK(physical == 48 || free_index >= previous_free_index,
          "free mapping is not monotonic");
    previous_request_index = request_index;
    previous_free_index = free_index;
  }
  CHECK(rift_heap_test_validate(), "empty heap failed validation");
  rift_heap_deinit();
}

static void optimized_zxn_mapping_matches_reference_exhaustively(void) {
  size_t physical;
  for (physical = 12u; physical <= 65532u; physical += 4u)
    CHECK(rift_heap_test_compare_zxn_maps(physical),
          "optimized ZXN request/free mapping diverged from reference");
}

static void poisoned_state_is_fully_initialised(void) {
  rift_heap_test_poison_state();
  memset(test_arena.bytes, 0xA5, sizeof(test_arena.bytes));
  CHECK(rift_heap_init(test_arena.bytes, 4096, 1),
        "init read poisoned allocator state");
  CHECK(rift_heap_test_validate(), "poisoned-state init left invalid indices");
  CHECK(rift_heap_try_alloc(17) != NULL,
        "allocation failed after poisoned-state init");
  CHECK(rift_heap_test_validate(), "post-poison allocation broke invariants");
  rift_heap_deinit();
}

static void fresh_frontier_uses_class_physical_bytes(void) {
  void *blocks[26];
  rift_heap_stats stats;
  /* LP64 raw physical is 272 bytes (32-byte header + 225-byte payload,
   * aligned to 16); its reusable class is 288. Twenty-five reusable blocks
   * plus one terminal exact-tail block therefore need exactly 7472 bytes. */
  init_heap(7472);
  rift_heap_stats_reset();
  for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
    blocks[i] = rift_heap_try_alloc(225);
    CHECK(blocks[i] != NULL,
          "class-rounded fresh allocations exhausted their exact cap");
  }
  stats = rift_heap_stats_get();
  CHECK(rift_heap_committed() == 25u * 288u + 272u,
        "fresh frontier did not preserve classes plus terminal exact tail");
  CHECK(stats.class_rounding_bytes == 25u * 16u,
        "frontier and terminal rounding were not reported separately");
  for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
    CHECK(rift_heap_try_free(blocks[i]) == RIFT_HEAP_FREE_OK,
          "fresh-frontier fixture did not free");
  CHECK(rift_heap_committed() == 0,
        "fresh-frontier fixture did not contract completely");
  rift_heap_deinit();

  init_heap(7456);
  for (size_t i = 0; i < 25; i++)
    CHECK(rift_heap_try_alloc(225) != NULL,
          "allocation below the terminal boundary failed early");
  CHECK(rift_heap_try_alloc(225) == NULL,
        "capacity below the measured minimum unexpectedly succeeded");
  rift_heap_deinit();
}

static void exact_fit_uses_the_complete_capacity(void) {
  const size_t capacities[] = {4096, 23392};
  for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
    size_t capacity = capacities[i];
    size_t payload_capacity;
    void *block;
    init_heap(capacity);
    CHECK(!rift_heap_set_limit(capacity - 16u),
          "permanent heap accepted a limit change");
    payload_capacity = capacity - rift_heap_block_overhead();
    CHECK(rift_heap_largest_free_payload() == payload_capacity,
          "largest satisfiable payload omitted the exact tail capacity");
    block = rift_heap_try_alloc(payload_capacity);
    CHECK(block != NULL, "exact maximum payload reported a false OOM");
    CHECK(rift_heap_committed() == capacity,
          "exact maximum payload did not consume exact capacity");
    CHECK(rift_heap_try_alloc(1) == NULL,
          "allocation beyond exact capacity unexpectedly succeeded");
    CHECK(rift_heap_try_free(block) == RIFT_HEAP_FREE_OK,
          "exact-fit block did not free");
    CHECK(rift_heap_committed() == 0,
          "free tail did not contract to zero");
    block = rift_heap_try_alloc(payload_capacity);
    CHECK(block != NULL,
          "exact maximum payload failed after complete contraction");
    CHECK(rift_heap_committed() == capacity,
          "repeated exact maximum payload did not consume capacity");
    CHECK(rift_heap_try_alloc(1) == NULL,
          "guard after repeated exact allocation unexpectedly succeeded");
    CHECK(rift_heap_try_free(block) == RIFT_HEAP_FREE_OK,
          "repeated exact-fit block did not free");
    CHECK(rift_heap_committed() == 0,
          "repeated exact-fit cycle did not contract to zero");
    CHECK(rift_heap_test_validate(), "exact-fit cycle broke invariants");
    rift_heap_deinit();
  }
}

static void expandable_limit_never_creates_a_raw_interior_block(void) {
  void *first;
  void *tail;
  void *retry;
  init_heap_mode(1024, 0);
  CHECK(rift_heap_set_limit(464), "failed to shrink the expandable limit");
  CHECK(rift_heap_try_alloc(432) == NULL,
        "shrunken expandable limit admitted a raw terminal block");
  CHECK(rift_heap_committed() == 0,
        "rejected expandable-tail allocation changed the frontier");
  CHECK(rift_heap_set_limit(1024), "failed to restore the heap limit");
  first = rift_heap_try_alloc(432); /* raw 464, reusable class 480 */
  tail = rift_heap_try_alloc(480);  /* reusable 512-byte class */
  CHECK(first && tail && rift_heap_committed() == 992,
        "restored-limit fixture did not preserve reusable classes");
  CHECK(rift_heap_try_free(first) == RIFT_HEAP_FREE_OK,
        "restored-limit interior block did not free");
  retry = rift_heap_try_alloc(432);
  CHECK(retry == first,
        "restored-limit block did not re-enter its request class");
  CHECK(rift_heap_try_free(retry) == RIFT_HEAP_FREE_OK,
        "restored-limit retry did not free");
  CHECK(rift_heap_try_free(tail) == RIFT_HEAP_FREE_OK,
        "safe terminal tail did not free");
  CHECK(rift_heap_committed() == 0,
        "restored-limit cycle did not contract completely");
  CHECK(rift_heap_test_validate(),
        "restored-limit cycle broke heap invariants");
  rift_heap_deinit();
}

static void class_rounding_and_top_class_are_exact(void) {
  void *smaller;
  void *guard1;
  void *fitting;
  void *guard2;
  void *selected;
  void *rounding_source;
  void *rounding_guard;
  void *top;
  void *top_guard;
  rift_heap_stats stats;
  init_heap(TEST_ARENA_BYTES);
  smaller = rift_heap_try_alloc(128); /* 160 physical */
  guard1 = rift_heap_try_alloc(1);
  fitting = rift_heap_try_alloc(144); /* 176 physical, same partial bin */
  guard2 = rift_heap_try_alloc(1);
  CHECK(smaller && guard1 && fitting && guard2, "partial-bin fixture failed");
  CHECK(rift_heap_try_free(smaller) == RIFT_HEAP_FREE_OK,
        "smaller partial-bin free failed");
  CHECK(rift_heap_try_free(fitting) == RIFT_HEAP_FREE_OK,
        "fitting partial-bin free failed");
  rift_heap_stats_reset();
  selected = rift_heap_try_alloc(140); /* rounds to 176 */
  CHECK(selected == fitting,
        "rounded request did not select its guaranteed fitting class");
  stats = rift_heap_stats_get();
  CHECK(stats.class_rounding_bytes == 0,
        "exact-class reuse reported nonexistent rounding");
  CHECK(rift_heap_try_free(selected) == RIFT_HEAP_FREE_OK,
        "selected partial-bin block did not free");
  CHECK(rift_heap_try_free(guard2) == RIFT_HEAP_FREE_OK, "guard2 free failed");
  CHECK(rift_heap_try_free(guard1) == RIFT_HEAP_FREE_OK, "guard1 free failed");
  CHECK(rift_heap_committed() == 0, "partial-bin fixture did not contract");

  rounding_source = rift_heap_try_alloc(241); /* 288 physical */
  rounding_guard = rift_heap_try_alloc(1);
  CHECK(rounding_source && rounding_guard, "reuse-rounding fixture failed");
  CHECK(rift_heap_try_free(rounding_source) == RIFT_HEAP_FREE_OK,
        "reuse-rounding source did not free");
  rift_heap_stats_reset();
  selected = rift_heap_try_alloc(225); /* 272 raw, charged to 288 on reuse */
  stats = rift_heap_stats_get();
  CHECK(selected == rounding_source,
        "rounded reclaimed-bin request did not reuse its fitting class");
  CHECK(stats.class_rounding_bytes == 16,
        "reclaimed-bin class rounding was not reported separately");
  CHECK(rift_heap_try_free(selected) == RIFT_HEAP_FREE_OK,
        "reuse-rounded block did not free");
  CHECK(rift_heap_try_free(rounding_guard) == RIFT_HEAP_FREE_OK,
        "reuse-rounding guard did not free");

  top = rift_heap_try_alloc(65000);
  top_guard = rift_heap_try_alloc(1);
  CHECK(top && top_guard, "top-class fixture did not fit its charged capacity");
  CHECK(rift_heap_try_free(top) == RIFT_HEAP_FREE_OK, "top block free failed");
  selected = rift_heap_try_alloc(65000);
  CHECK(selected == top,
        "top-class request did not reuse its charged free block");
  CHECK(rift_heap_try_free(selected) == RIFT_HEAP_FREE_OK,
        "top-class selected block did not free");
  CHECK(rift_heap_try_free(top_guard) == RIFT_HEAP_FREE_OK,
        "top guard did not free");
  CHECK(rift_heap_test_validate(), "partial/top-bin search broke invariants");
  rift_heap_deinit();
}

static void split_and_three_way_coalescing_are_immediate(void) {
  void *blocks[5];
  void *split_source;
  void *split_guard;
  void *split_result;
  rift_heap_stats stats;
  init_heap(8192);
  split_source = rift_heap_try_alloc(1000);
  split_guard = rift_heap_try_alloc(1);
  CHECK(split_source && split_guard, "split fixture allocation failed");
  CHECK(rift_heap_try_free(split_source) == RIFT_HEAP_FREE_OK,
        "split source did not free");
  rift_heap_stats_reset();
  split_result = rift_heap_try_alloc(100);
  stats = rift_heap_stats_get();
  CHECK(split_result == split_source, "split did not reuse the fitting block");
  CHECK(stats.splits == 1, "fitting free block was not split exactly once");
  CHECK(rift_heap_test_validate(), "split broke boundary/tree invariants");
  CHECK(rift_heap_try_free(split_result) == RIFT_HEAP_FREE_OK,
        "split result did not free");
  CHECK(rift_heap_try_free(split_guard) == RIFT_HEAP_FREE_OK,
        "split guard did not free");

  for (int i = 0; i < 5; i++) blocks[i] = rift_heap_try_alloc(64);
  for (int i = 0; i < 5; i++) CHECK(blocks[i] != NULL, "coalesce fixture failed");
  CHECK(rift_heap_try_free(blocks[1]) == RIFT_HEAP_FREE_OK, "left free failed");
  CHECK(rift_heap_try_free(blocks[3]) == RIFT_HEAP_FREE_OK, "right free failed");
  rift_heap_stats_reset();
  CHECK(rift_heap_try_free(blocks[2]) == RIFT_HEAP_FREE_OK, "middle free failed");
  stats = rift_heap_stats_get();
  CHECK(stats.coalesces == 2, "free did not merge both adjacent neighbours");
  CHECK(rift_heap_largest_free_payload() >= 250,
        "three-way merge did not expose the combined payload");
  CHECK(rift_heap_test_validate(), "three-way coalesce broke invariants");
  rift_heap_deinit();
}

static void oom_is_atomic_and_fragmentation_is_classified(void) {
  void *blocks[10];
  size_t digest;
  rift_heap_stats stats;
  init_heap(1024);
  for (int i = 0; i < 10; i++) {
    blocks[i] = rift_heap_try_alloc(64);
    CHECK(blocks[i] != NULL, "fragmentation fixture allocation failed");
    memset(blocks[i], i + 1, 64);
  }
  for (int i = 0; i < 10; i += 2)
    CHECK(rift_heap_try_free(blocks[i]) == RIFT_HEAP_FREE_OK,
          "fragmentation fixture free failed");
  CHECK(rift_heap_test_validate(), "alternating frees broke invariants");
  digest = rift_heap_test_digest();
  rift_heap_stats_reset();
  CHECK(rift_heap_try_alloc(200) == NULL,
        "fragmented heap unexpectedly satisfied a non-fitting request");
  stats = rift_heap_stats_get();
  CHECK(stats.fragmentation_failures == 1 && stats.capacity_failures == 0,
        "OOM was not classified as fragmentation");
  CHECK(rift_heap_test_digest() == digest,
        "failed allocation mutated heap state or live contents");
  for (int i = 1; i < 10; i += 2) {
    unsigned char *payload = blocks[i];
    for (int j = 0; j < 64; j++)
      CHECK(payload[j] == (unsigned char)(i + 1),
            "OOM rollback changed live payload bytes");
  }
  rift_heap_deinit();
}

static void deterministic_double_free_precedes_mutation(void) {
  void *first;
  void *guard;
  size_t digest;
  init_heap(512);
  first = rift_heap_try_alloc(32);
  guard = rift_heap_try_alloc(32);
  CHECK(first && guard, "double-free fixture allocation failed");
  CHECK(rift_heap_try_free(first) == RIFT_HEAP_FREE_OK,
        "first free was rejected");
  digest = rift_heap_test_digest();
  CHECK(rift_heap_try_free(first) == RIFT_HEAP_FREE_DOUBLE,
        "double free was not diagnosed deterministically");
  CHECK(rift_heap_test_digest() == digest,
        "double-free rejection mutated allocator state");
  CHECK(rift_heap_try_free(guard) == RIFT_HEAP_FREE_OK,
        "guard free failed after double-free rejection");
  rift_heap_deinit();
}

static void adversarial_bin_insert_delete_preserves_invariants(void) {
  enum { BLOCK_COUNT = 96 };
  void *blocks[BLOCK_COUNT];
  void *guards[BLOCK_COUNT];
  size_t payloads[BLOCK_COUNT];
  unsigned int order[BLOCK_COUNT];
  unsigned int seed = 0x31415926u;
  init_heap(TEST_ARENA_BYTES);
  for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
    payloads[i] = 24u + (size_t)((i * 73u) % 1400u);
    blocks[i] = rift_heap_try_alloc(payloads[i]);
    guards[i] = rift_heap_try_alloc(1);
    order[i] = i;
    CHECK(blocks[i] && guards[i], "bin stress fixture allocation failed");
  }
  for (unsigned int i = BLOCK_COUNT - 1; i > 0; i--) {
    unsigned int swap;
    seed = seed * 1103515245u + 12345u;
    swap = seed % (i + 1u);
    unsigned int temporary = order[i];
    order[i] = order[swap];
    order[swap] = temporary;
  }
  for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
    CHECK(rift_heap_try_free(blocks[order[i]]) == RIFT_HEAP_FREE_OK,
          "bin stress free failed");
    CHECK(rift_heap_test_validate(),
          "bin invariants failed during adversarial insertion");
  }
  rift_heap_stats_reset();
  for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
    blocks[i] = rift_heap_try_alloc(payloads[order[i]]);
    CHECK(blocks[i] != NULL, "bin stress reallocation reported false OOM");
    CHECK(rift_heap_test_validate(),
          "bin invariants failed during adversarial deletion/split");
  }
  CHECK(rift_heap_stats_get().allocation_bin_visits <= BLOCK_COUNT,
        "constant-time bin lookup performed more than one visit per alloc");
  rift_heap_deinit();
}

int main(void) {
  printf("Rift segregated allocator tests\n\n");
  RUN(target_layout_and_exhaustive_bin_mapping);
  RUN(optimized_zxn_mapping_matches_reference_exhaustively);
  RUN(poisoned_state_is_fully_initialised);
  RUN(fresh_frontier_uses_class_physical_bytes);
  RUN(exact_fit_uses_the_complete_capacity);
  RUN(expandable_limit_never_creates_a_raw_interior_block);
  RUN(class_rounding_and_top_class_are_exact);
  RUN(split_and_three_way_coalescing_are_immediate);
  RUN(oom_is_atomic_and_fragmentation_is_classified);
  RUN(deterministic_double_free_precedes_mutation);
  RUN(adversarial_bin_insert_delete_preserves_invariants);
  printf("\n%d/%d passed\n", tests_run - tests_failed, tests_run);
  return tests_failed ? 1 : 0;
}
