# ZXN allocator timing fixtures

These are target benchmarks, not assertion tests. Build with `--debug` so Z88DK
retains a map, look up the marker function, gate label, and the first label
after the gate, then pass their addresses to `rock-emu measure`.

The primitive and control fixtures invoke the same marker three times: the
gate, the workload, and the endpoint. Use `--breakpoint-pass-count 2` to stop
at the endpoint. The string fixture resets statistics immediately after its
gate, then uses a post-workload statistics marker and an endpoint, so it also
uses a count of two. Always supply
`--gate-address`: the launcher uses it as an initial breakpoint and resets the
T-state counter only after that breakpoint has stopped the CPU. `--release-pc`
sets the program counter to the label immediately after the gate while it is
paused. This avoids timing a memory-write protocol action and is reliable with
the ZX Next's paged memory map.

Example:

```sh
./rock --target=zxn --debug \
  test/benchmarks/allocator_primitive_cycle_bench.rkr /private/tmp/allocator-primitive.exe
rg 'allocator_primitive_benchmark|l_allocator_primitive_benchmark_0010[15]' /private/tmp/allocator-primitive.map
tools/rock-emu measure --target zxn /private/tmp/allocator-primitive.nex \
  --gate-address 0x7146 --breakpoint 0x7136 --breakpoint-pass-count 2 \
  --release-pc 0x714d --capture-memory 0xa9cf:16
```

Addresses are link outputs: always read them from the map of the artifact just
built. See `tools/emulators/zxn/zesarux/0002-zrcp-headless-cpu-step.patch` and
`0003-zrcp-tstate-reset-ack.patch` for the required headless ZEsarUX support.
The eight captured 16-bit words are, in
order: allocations, frees, allocation-list scans, free-list scans, coalesces,
magazine hits, buddy splits, and explicit collections. The loop-control fixture
mirrors the statistics reset, read, and stores so subtraction isolates
allocator work.

The normal target build keeps per-operation allocator counters out of the
fast path. Build the same fixture with `--allocator-stats` when collecting the
separate structural proof (zero heap-list scans, magazine hits, splits, and
collections). Do not use that instrumented build for the cycle budget.
