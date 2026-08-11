# Target Emulator Tools

`rock-emu` runs built target artifacts through project-owned emulator adapters.
The first supported pairing is ZX Spectrum Next (`zxn`) with ZEsarUX.

```sh
export ZESARUX_BIN=/path/to/zesarux
tools/rock-emu run --target zxn program.nex
tools/rock-emu inspect --target zxn program.nex
tools/rock-emu test --target zxn program.nex
```

`run` reports whether the emulator process completed. `inspect` keeps VM
diagnostics. `test` requires a `.nex` built with `rock --zxn-test` and accepts
only a `ROCKTEST:FINISH` marker with no `ROCKTEST:FAIL` marker as success.

The command prints one JSON result to stdout. Its `artifacts` path contains the
emulator command and logs, and on failure or inspection, screen BMP/OCR,
registers, stack, t-states, memory-page state, the first 128 bytes at the NEX
entry address and current program counter, and a ZSF snapshot when ZRCP can save one. Open `screen.bmp`
directly when visual inspection is needed. Pass `--keep-all` to retain a copy
of the supplied NEX beside those diagnostics.

The ZEsarUX adapter requires a build that supports
`--remoteprotocol-host`; it always binds ZRCP to `127.0.0.1`. This is required
because ZRCP is unauthenticated. The local ZEsarUX source under
`/Users/blater/retro/retro1/zesarux` includes the corresponding patch.
For another checkout, apply
[`0001-zrcp-loopback-bind.patch`](emulators/zxn/zesarux/0001-zrcp-loopback-bind.patch)
before rebuilding the emulator.

To build an instrumented test artifact manually:

```sh
./rock --zxn-test --target=zxn test/simple_test.rkr /private/tmp/simple.exe
tools/rock-emu test --target=zxn /private/tmp/simple.nex
```

`--zxn-test` is test-only. It activates `zxn_test.c`, has `Assert.rkr` emit
PASS/FAIL markers, and asks ZEsarUX to exit once the generated `main` returns.
Normal Rock programs do not emit emulator debug-port traffic.

`test` is deliberately strict: a timeout, malformed marker stream, missing
`BEGIN`/`FINISH`, non-zero reported failure count, or non-zero emulator exit
is a failure. This distinguishes “a NEX was built” from “the generated program
booted and completed.”

## Curated ZXN execution suite

`tools/test-zxn` provides reproducible broader target coverage without changing
the host-only `run_tests.sh` harness. It builds every entry in
[`zxn-test-suite.txt`](zxn-test-suite.txt) using `--target=zxn --zxn-test`, then
requires ZEsarUX to observe the complete test protocol for every artifact.

```sh
export ZESARUX_BIN=/path/to/zesarux
tools/test-zxn
tools/test-zxn test/array_test.rkr
```

Pass `--emulator-bin /path/to/zesarux` instead of setting the environment
variable, `--timeout-seconds N` for slower debugging sessions, and
`--keep-builds` to retain the intermediate NEX files. Per-case compiler logs,
launcher logs, retained NEX copies, and failure diagnostics are stored under
`test-artifacts/zxn/` (ignored by Git). The suite requires a real emulator; it
will not report success merely because ZEsarUX is unavailable.
