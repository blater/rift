# ZXN allocator timing fixtures

These are target benchmarks, not assertion tests. Build with `--debug` so Z88DK
retains a map, look up the marker function, gate label, and the first label
after the gate, then pass their addresses to `rift-emu measure`.

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
./rift --target=zxn --debug \
  test/benchmarks/allocator_primitive_cycle_bench.rift /private/tmp/allocator-primitive.exe
# Use the reported /tmp/rift-build-* directory for the map, then:
rg 'allocator_primitive_benchmark|l_allocator_primitive_benchmark_0010[15]' /tmp/rift-build-*/allocator-primitive.map
tools/rift-emu measure --target zxn /private/tmp/allocator-primitive.nex \
  --gate-address 0x7146 --breakpoint 0x7136 --breakpoint-pass-count 2 \
  --release-pc 0x714d --capture-memory 0xa9cf:16
```

Addresses are link outputs: always read them from the map of the artifact just
built. The canonical `/Users/blater/src/zesarux` checkout carries the required
headless and acknowledged-reset support on `integration/zrcp-automation`; verify
it with `tools/test-zesarux-zrcp` before collecting measurements.
As of 2026-08-15, the current endpoint run can hang after those two protocol
acknowledgements with both the legacy and canonical emulator binaries. Treat
that as a separate launcher/benchmark issue, not as a failed ZRCP patch check.
The eight captured 16-bit words are, in
order: allocations, frees, allocation-bin visits, free-bin visits, coalesces,
splits, class-rounding bytes, and explicit collections. The loop-control fixture
mirrors the statistics reset, read, and stores so subtraction isolates
allocator work.

The normal target build keeps per-operation allocator counters out of the
fast path. Build the same fixture with `--allocator-stats` when collecting the
separate structural proof (constant-time bin visits, splits, rounding, and
collections). Do not use that instrumented build for the cycle budget.
