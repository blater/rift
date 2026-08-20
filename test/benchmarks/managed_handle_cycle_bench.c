/*
 * ZX Next stable-handle timing fixture.
 *
 * Build the same source as either a managed or raw-pointer control and select
 * one workload with RIFT_HANDLE_BENCH_MODE.  Initialisation, allocation, cache
 * priming, and cleanup sit outside the measured interval.  The emulator stops
 * at managed_handle_cycle_gate, resets its partial T-state counter, resumes at
 * managed_handle_cycle_after_gate, and stops on the second subsequent entry to
 * managed_handle_cycle_marker.
 */
#include "managed_heap.h"
#include "pools.h"

#include <stdint.h>

#define RIFT_HANDLE_BENCH_CACHE 1
#define RIFT_HANDLE_BENCH_FUSED_ACCESS 2
#define RIFT_HANDLE_BENCH_FUSED_PIN 3

#ifndef RIFT_HANDLE_BENCH_MODE
#define RIFT_HANDLE_BENCH_MODE RIFT_HANDLE_BENCH_CACHE
#endif

#ifndef RIFT_HANDLE_BENCH_RAW
#define RIFT_HANDLE_BENCH_RAW 0
#endif

#define RIFT_HANDLE_BENCH_ITERATIONS 128u
#define RIFT_HANDLE_BENCH_TYPE 1u

typedef struct handle_bench_payload {
  uint16_t words[8];
} handle_bench_payload;

const managed_static_entry rift_managed_static_table[] = {{0, 0}};
const size_t rift_managed_static_count = 0;

static managed_ref benchmark_ref;
static handle_bench_payload *benchmark_raw;
static volatile uint16_t benchmark_sink;

#if RIFT_HANDLE_BENCH_RAW
typedef struct raw_access_token {
  void *payload;
} raw_access_token;

/* The raw control crosses the same two-word callee-cleanup facade as the
 * managed accessor.  It differs only in resolving a raw pointer rather than
 * a stable identity through the managed cache. */
void raw_access_begin(void *payload, raw_access_token *token) __z88dk_callee {
  token->payload = payload;
}
#else
static void benchmark_destroy(managed_type_id type, void *payload) {
  (void)type;
  (void)payload;
}

static void benchmark_fault(managed_fault_reason reason, size_t requested) {
  (void)reason;
  (void)requested;
  for (;;) {
  }
}
#endif

static void run_field_loop(handle_bench_payload *payload) {
  uint16_t index;
  for (index = 0; index < RIFT_HANDLE_BENCH_ITERATIONS; index++) {
    uint16_t slot = (uint16_t)(index & 7u);
    benchmark_sink =
        (uint16_t)(benchmark_sink + payload->words[slot] + slot);
  }
}

void managed_handle_cycle_marker(unsigned char phase) {
  if (phase == 0) {
#asm
    PUBLIC _managed_handle_cycle_gate
._managed_handle_cycle_gate
    jr _managed_handle_cycle_gate
    PUBLIC _managed_handle_cycle_after_gate
._managed_handle_cycle_after_gate
#endasm
  }

  if (phase != 1) return;

#if RIFT_HANDLE_BENCH_MODE == RIFT_HANDLE_BENCH_CACHE
  {
    uint16_t index;
#if RIFT_HANDLE_BENCH_RAW
    raw_access_token access;
    for (index = 0; index < RIFT_HANDLE_BENCH_ITERATIONS; index++) {
      handle_bench_payload *payload;
      raw_access_begin(benchmark_raw, &access);
      payload = (handle_bench_payload *)access.payload;
      benchmark_sink = (uint16_t)(benchmark_sink + payload->words[index & 7u]);
      access.payload = 0;
    }
#else
    managed_access_token access;
    for (index = 0; index < RIFT_HANDLE_BENCH_ITERATIONS; index++) {
      handle_bench_payload *payload;
      managed_access_begin(benchmark_ref, &access);
      payload = (handle_bench_payload *)managed_access_ptr(&access);
      benchmark_sink = (uint16_t)(benchmark_sink + payload->words[index & 7u]);
      managed_access_end(&access);
    }
#endif
  }
#elif RIFT_HANDLE_BENCH_MODE == RIFT_HANDLE_BENCH_FUSED_ACCESS
#if RIFT_HANDLE_BENCH_RAW
  run_field_loop(benchmark_raw);
#else
  {
    managed_access_token access;
    managed_access_begin(benchmark_ref, &access);
    run_field_loop((handle_bench_payload *)managed_access_ptr(&access));
    managed_access_end(&access);
  }
#endif
#elif RIFT_HANDLE_BENCH_MODE == RIFT_HANDLE_BENCH_FUSED_PIN
#if RIFT_HANDLE_BENCH_RAW
  run_field_loop(benchmark_raw);
#else
  {
    managed_pin_token pin;
    managed_pin_typed(benchmark_ref, RIFT_HANDLE_BENCH_TYPE, &pin);
    run_field_loop((handle_bench_payload *)managed_pin_ptr(&pin));
    managed_unpin_typed(&pin);
  }
#endif
#else
#error unknown RIFT_HANDLE_BENCH_MODE
#endif
}

int main(void) {
  handle_bench_payload *payload;
  uint16_t index;

  rift_pools_init(0);
#if RIFT_HANDLE_BENCH_RAW
  benchmark_raw =
      (handle_bench_payload *)rift_longlived_alloc(sizeof(*benchmark_raw));
  payload = benchmark_raw;
#else
  {
    managed_heap_options options;
    managed_access_token access;
    options.destroy = benchmark_destroy;
    options.fault = benchmark_fault;
    managed_heap_init(&options);
    benchmark_ref = managed_alloc(sizeof(handle_bench_payload));
    managed_access_begin(benchmark_ref, &access);
    payload = (handle_bench_payload *)managed_access_ptr(&access);
    /* This access also primes the direct-mapped identity cache. */
    for (index = 0; index < 8u; index++) payload->words[index] = index + 1u;
    managed_access_end(&access);
  }
#endif
#if RIFT_HANDLE_BENCH_RAW
  for (index = 0; index < 8u; index++) payload->words[index] = index + 1u;
#endif

  benchmark_sink = 0;
  managed_handle_cycle_marker(0);
  managed_handle_cycle_marker(1);
  managed_handle_cycle_marker(2);

#if RIFT_HANDLE_BENCH_RAW
  rift_longlived_free(benchmark_raw);
#else
  managed_release_typed(benchmark_ref, RIFT_HANDLE_BENCH_TYPE);
  managed_heap_deinit();
#endif
  rift_pools_deinit();
  return benchmark_sink == 0xffffu;
}
