#include "../src/lib/managed_heap.h"
#include "../src/lib/segregated_heap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_ARENA_BYTES 131072u
#define TYPE_TRIVIAL 1u
#define MAX_SLICES 128u

_Alignas(16) static unsigned char test_arena[TEST_ARENA_BYTES];

const managed_static_entry rift_managed_static_table[] = {{NULL, 0}};
const size_t rift_managed_static_count = 0;

static int tests_run;
static int tests_failed;

#define CHECK(condition, message)                                           \
  do {                                                                      \
    if (!(condition)) {                                                     \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
      tests_failed++;                                                       \
      return;                                                               \
    }                                                                       \
  } while (0)

#define RUN(test)                                              \
  do {                                                         \
    int before = tests_failed;                                 \
    tests_run++;                                               \
    test();                                                    \
    if (tests_failed == before) printf("  ok %s\n", #test);  \
  } while (0)

static void test_destroy(managed_type_id type, void *payload) {
  (void)type;
  (void)payload;
}

static void test_fault(managed_fault_reason reason, size_t requested) {
  fprintf(stderr, "unexpected managed fault %d (%zu)\n", (int)reason,
          requested);
  tests_failed++;
}

static void init_runtime(void) {
  managed_heap_options options;
  memset(test_arena, 0xa5, sizeof(test_arena));
  if (!rift_heap_init(test_arena, sizeof(test_arena), 1)) {
    fprintf(stderr, "FAIL: heap init rejected compactor arena\n");
    tests_failed++;
    return;
  }
  options.destroy = test_destroy;
  options.fault = test_fault;
  managed_heap_init(&options);
}

static void deinit_runtime(void) {
  managed_heap_deinit();
  rift_heap_deinit();
}

static void *payload_address(managed_ref ref) {
  managed_access_token access;
  void *payload;
  managed_access_begin(ref, &access);
  payload = managed_access_ptr(&access);
  managed_access_end(&access);
  return payload;
}

static void write_word(managed_ref ref, uint16_t value) {
  managed_access_token access;
  managed_access_begin(ref, &access);
  *(uint16_t *)managed_access_ptr(&access) = value;
  managed_access_end(&access);
}

static uint16_t read_word(managed_ref ref) {
  managed_access_token access;
  uint16_t value;
  managed_access_begin(ref, &access);
  value = *(uint16_t *)managed_access_ptr(&access);
  managed_access_end(&access);
  return value;
}

static void finish_routine_pass(void) {
  unsigned int slices = 0;
  while (rift_managed_maintenance_due && slices++ < MAX_SLICES) {
    rift_heap_compact_report report;
    rift_heap_compact_status status = managed_heap_compact_routine(&report);
    if (status == RIFT_HEAP_COMPACT_INVALID) {
      fprintf(stderr, "FAIL: routine compactor rejected a valid heap\n");
      tests_failed++;
      return;
    }
  }
  if (rift_managed_maintenance_due) {
    fprintf(stderr, "FAIL: routine compactor did not finish a bounded pass\n");
    tests_failed++;
  }
}

static void no_debt_is_zero_work(void) {
  rift_heap_compact_report report;
  init_runtime();
  CHECK(managed_heap_compact_routine(&report) == RIFT_HEAP_COMPACT_IDLE,
        "no-debt compaction was not idle");
  CHECK(report.headers_scanned == 0 && report.moves == 0 &&
            report.copied_bytes == 0,
        "no-debt compaction performed work");
  managed_maintenance_safepoint();
  CHECK(!rift_managed_maintenance_due,
        "no-debt facade raised maintenance work");
  deinit_runtime();
}

static void pin_topology_invalidates_and_restarts_cursor(void) {
  managed_ref refs[20];
  managed_pin_token pin;
  managed_pin_token nested;
  rift_heap_compact_report report;
  uint16_t epoch;
  size_t first_scan;
  unsigned int index;

  init_runtime();
  for (index = 0; index < 20; index++) refs[index] = managed_alloc(2);
  epoch = rift_heap_test_movement_epoch();
  managed_pin_typed(refs[19], TYPE_TRIVIAL, &pin);
  CHECK(rift_heap_test_movement_epoch() == (uint16_t)(epoch + 1u),
        "pin 0->1 did not advance topology epoch");
  CHECK(rift_heap_test_compaction_debt() == 1u,
        "pin 0->1 did not raise one debt unit");
  epoch = rift_heap_test_movement_epoch();
  managed_pin_typed(refs[19], TYPE_TRIVIAL, &nested);
  CHECK(rift_heap_test_movement_epoch() == epoch &&
            rift_heap_test_compaction_debt() == 1u,
        "nested pin changed movement topology");
  CHECK(managed_heap_compact_routine(&report) == RIFT_HEAP_COMPACT_PROGRESS &&
            report.headers_scanned == RIFT_HEAP_ROUTINE_HEADER_BUDGET &&
            report.moves == 0,
        "first topology scan did not stop at the header budget");
  first_scan = rift_heap_test_cursor_scan();
  managed_unpin_typed(&nested);
  CHECK(rift_heap_test_movement_epoch() == epoch,
        "pin 2->1 changed movement topology");
  managed_unpin_typed(&pin);
  CHECK(rift_heap_test_movement_epoch() == (uint16_t)(epoch + 1u),
        "unpin 1->0 did not advance topology epoch");
  CHECK(managed_heap_compact_routine(&report) == RIFT_HEAP_COMPACT_PROGRESS &&
            rift_heap_test_cursor_scan() == first_scan,
        "stale cursor did not restart at the region base");

  rift_heap_test_set_movement_epoch(UINT16_MAX);
  managed_pin_typed(refs[18], TYPE_TRIVIAL, &pin);
  CHECK(rift_heap_test_movement_epoch() == 0,
        "topology epoch did not restart at wrap");
  CHECK(rift_heap_test_cursor_scan() == (size_t)-1,
        "epoch wrap did not invalidate the cursor");
  managed_unpin_typed(&pin);
  finish_routine_pass();
  for (index = 0; index < 20; index++)
    managed_release_typed(refs[index], TYPE_TRIVIAL);
  deinit_runtime();
}

static void pin_barrier_does_not_block_later_interval(void) {
  managed_ref first;
  managed_ref pinned;
  managed_ref hole;
  managed_ref moved;
  managed_pin_token pin;
  rift_heap_compact_report report;
  void *pin_before;
  void *move_before;
  size_t committed_before;

  init_runtime();
  first = managed_alloc(2);
  pinned = managed_alloc(2);
  hole = managed_alloc(2);
  moved = managed_alloc(2);
  write_word(pinned, 0x1234u);
  write_word(moved, 0x5678u);
  managed_pin_typed(pinned, TYPE_TRIVIAL, &pin);
  pin_before = managed_pin_ptr(&pin);
  move_before = payload_address(moved);
  managed_release_typed(first, TYPE_TRIVIAL);
  managed_release_typed(hole, TYPE_TRIVIAL);
  committed_before = rift_heap_committed();

  CHECK(managed_heap_compact_routine(&report) ==
            RIFT_HEAP_COMPACT_PASS_COMPLETE,
        "pin-separated slice did not complete at the contracted tail");
  CHECK(report.headers_scanned <= RIFT_HEAP_ROUTINE_HEADER_BUDGET &&
            report.moves == 1 && report.copied_bytes <= 192u &&
            report.pin_barriers == 1 && report.bin_edits == 1 &&
            report.frontier_writes == 2,
        "pin-separated slice exceeded or misreported its work budget");
  CHECK(managed_pin_ptr(&pin) == pin_before && read_word(pinned) == 0x1234u,
        "pinned object moved or changed");
  CHECK(payload_address(moved) != move_before && read_word(moved) == 0x5678u,
        "later interval did not move with stable identity/content");
  CHECK(rift_heap_committed() < committed_before &&
            report.tail_contracted != 0,
        "routine move did not contract the free tail immediately");
  CHECK(rift_heap_test_validate(),
        "pin-separated move broke bins or boundary tags");

  managed_unpin_typed(&pin);
  finish_routine_pass();
  managed_release_typed(pinned, TYPE_TRIVIAL);
  managed_release_typed(moved, TYPE_TRIVIAL);
  deinit_runtime();
}

static void internal_page_move_repairs_backlinks(void) {
  managed_ref refs[17];
  rift_heap_compact_report report;
  void *root_before;
  void *root_after;
  unsigned int index;
  unsigned int slices = 0;

  init_runtime();
  for (index = 0; index < 17; index++) {
    refs[index] = managed_alloc(2);
    write_word(refs[index], (uint16_t)(0x4000u + index));
  }
  root_before = managed_heap_test_root_address();
  managed_release_typed(refs[15], TYPE_TRIVIAL);
  refs[15] = 0;
  root_after = root_before;
  while (root_after == root_before && rift_managed_maintenance_due &&
         slices++ < MAX_SLICES) {
    CHECK(managed_heap_compact_routine(&report) != RIFT_HEAP_COMPACT_INVALID,
          "page compaction rejected a valid adaptive directory");
    CHECK(report.headers_scanned <= RIFT_HEAP_ROUTINE_HEADER_BUDGET &&
              report.moves <= 1 && report.copied_bytes <= 192u &&
              report.index_reads <= 16u && report.index_writes <= 17u,
          "page move exceeded a hard routine budget");
    root_after = managed_heap_test_root_address();
  }
  CHECK(root_after != root_before,
        "adaptive16 root page did not move into the preceding hole");
  CHECK(rift_heap_test_validate(),
        "adaptive16 page move broke physical heap invariants");
  for (index = 0; index < 17; index++) {
    if (!refs[index]) continue;
    CHECK(read_word(refs[index]) == (uint16_t)(0x4000u + index),
          "page move broke a stable object lookup");
  }
  finish_routine_pass();
  for (index = 0; index < 17; index++)
    if (refs[index]) managed_release_typed(refs[index], TYPE_TRIVIAL);
  deinit_runtime();
}

int main(void) {
  printf("managed routine compactor tests\n");
  RUN(no_debt_is_zero_work);
  RUN(pin_topology_invalidates_and_restarts_cursor);
  RUN(pin_barrier_does_not_block_later_interval);
  RUN(internal_page_move_repairs_backlinks);
  printf("%d tests, %d failures\n", tests_run, tests_failed);
  return tests_failed ? 1 : 0;
}
