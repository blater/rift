# Target Emulator Tools

`rift-emu` runs built target artifacts through project-owned emulator adapters.
The first supported pairing is ZX Spectrum Next (`zxn`) with ZEsarUX.

```sh
export ZESARUX_BIN=/path/to/zesarux
tools/rift-emu run --target zxn program.nex
tools/rift-emu inspect --target zxn program.nex
tools/rift-emu test --target zxn program.nex
```

Verify that a ZEsarUX build provides Rift's complete loopback/headless/timing
ZRCP contract before using it for target evidence:

```sh
tools/test-zesarux-zrcp --emulator-bin /path/to/zesarux
```

The probe requires the listener to bind to `127.0.0.1`, enters and exits CPU
step mode with the null video driver, checks the acknowledged partial t-state
reset, and requires a clean emulator shutdown. Routine emulator output is
captured, so it does not need a quiet-output patch.

On macOS and Linux, ZEsarUX keeps `zesaruxrc` and the generated
`zesarux_index_menu.idx` under `~/.zesar/`. It migrates both legacy files
from the root of the home directory on startup. Windows retains its existing
home-directory behavior. Verify fresh-home and migration behavior with
`make test-zesarux-index-cache`. Headless Rift automation passes
`--disable-search-menu`, so it neither needs nor rewrites the interactive index.

`maintain-zesarux-fork` manages the canonical fork as one ordered, atomic patch
stack. It keeps `main` as a clean upstream mirror, rebases the maintained stack
once per upstream refresh, runs the full headless gates before publication, and
creates temporary contribution branches only when a patch is being prepared
for upstream:

```sh
tools/maintain-zesarux-fork status
tools/maintain-zesarux-fork refresh
tools/maintain-zesarux-fork validate
tools/maintain-zesarux-fork publish zrcp-headless-YYYY-MM-DD-N
tools/maintain-zesarux-fork contribution contribution/topic <commit>
```

`refresh` never pushes. `publish` reruns validation, uses
`--force-with-lease`, and requires a new immutable tag. The checkout defaults
to the `zesarux` directory beside Rift and may be overridden with
`ZESARUX_SOURCE`.

`measure` is the target-cycle mode. It first stops a benchmark at a known gate
with a real breakpoint, resets ZEsarUX's partial T-state counter only after
that pause is acknowledged, sets the PC just after the gate, and stops on a
breakpoint. This avoids relying on a debugger memory write across the target's
paged address space.
`--breakpoint-pass-count 2` is useful when the same marker wraps
both the workload and its endpoint.

```sh
tools/rift-emu measure --target zxn program.nex \
  --gate-address 0x7146 --breakpoint 0x7136 --breakpoint-pass-count 2 \
  --release-pc 0x714d
```

It requires the loopback-bind, headless CPU-step, and acknowledged-counter-reset
commits on the canonical ZEsarUX fork's `integration/zrcp-automation` branch.
The validated baseline is tag `zrcp-headless-2026-08-15` at commit
`4a5c08189e1c6406d50f9d565ea44cc156a4d7f9`. Run
`tools/test-zesarux-zrcp` to verify that contract independently.

The maintained fork also provides `--silent` (short form `-s`) for
informational startup UI. Rift's launcher uses that single option instead of
separate welcome and splash switches. It suppresses startup update checks,
version/change notices, first-run help, memorial notices, and joystick notices;
warnings, errors, crashes, and ZRCP diagnostics remain visible. Verify the
contract with `make test-zesarux-silent-startup`.

`--noupdates` disables every statistics-server path: version lookup, yesterday
user-count lookup, telemetry UUID generation/submission, and the telemetry
consent prompt. Rift automation and `~/bin/zx` pass it alongside `--silent`.
It does not disable user-requested online features such as ZENG or the online
game/search browsers. Verify it with `make test-zesarux-noupdates`.

`run` reports whether the emulator process completed. `inspect` keeps VM
diagnostics. `test` requires a `.nex` built with `rift --zxn-test` and accepts
only a `RIFTTEST:FINISH` marker with no `RIFTTEST:FAIL` marker as success.

The command prints one JSON result to stdout. Its `artifacts` path contains the
emulator command and logs, and on failure or inspection, screen BMP/OCR,
registers, stack, t-states, memory-page state, the first 128 bytes at the NEX
entry address and current program counter, and a ZSF snapshot when ZRCP can
save one. Open `screen.bmp` directly when visual inspection is needed. Pass
`--keep-all` to retain a copy of the supplied NEX beside those diagnostics.

The ZEsarUX adapter requires a build that supports
`--remoteprotocol-host`; it always binds ZRCP to `127.0.0.1`. This is required
because ZRCP is unauthenticated. The local ZEsarUX source under
`/Users/blater/src/zesarux` is the canonical version-controlled fork; build and
test its `integration/zrcp-automation` branch before collecting Rift evidence.

To build an instrumented test artifact manually:

```sh
./rift --zxn-test --target=zxn test/simple_test.rift /private/tmp/simple.exe
tools/rift-emu test --target=zxn /private/tmp/simple.nex
```

`--zxn-test` is test-only. It activates `zxn_test.c`, has `Assert.rift` emit
PASS/FAIL markers, and asks ZEsarUX to exit once the generated `main` returns.

For an input-waiting test, pass `--send-keys-ascii` a comma-separated list of
ASCII codes. The launcher injects them through ZEsarUX after the NEX has loaded
and batches long sequences within ZRCP's parameter limit. For example,
`--send-keys-ascii 49,50,51,13` types `123` followed by Enter. Deterministic
tests should write a ready byte immediately before waiting for input and name
it with `--send-keys-after-memory ADDRESS:VALUE` so the first press cannot race
startup.

`--allocator-stats` enables per-operation allocator counters for target
diagnostics. They are intentionally disabled in ordinary ZXN builds so timing
measurements represent the runtime fast path rather than instrumentation.
Normal Rift programs do not emit emulator debug-port traffic.

Assertion markers include the Rift assertion description, and the launcher's
JSON result exposes the boot stages plus observed pass/fail descriptions. This
makes an execution failure diagnosable without relying on the emulator's text
screen.

## ZXN memory-aware linking

ZXN builds link only the runtime components referenced by generated C. This is
the default `--rtl=auto` behaviour and leaves the largest possible gap between
linked data/BSS and the protected high stack for automatic managed memory. Use
`--rtl=all` only when hand-written embedded C calls an RTL symbol that the
generated code cannot expose to the linker; it is a diagnostic mode that can
shrink the managed gap enough for a build-time minimum check or runtime demand
to fail.

The target begins at `$5B00`; the shared pragma exports stack top `$FF58` and a
2 KiB stack size. After linking, the managed arena begins at aligned
`__BSS_END_tail` and ends at the aligned stack floor, less any explicit reserve
or total cap. No fixed bump or long-lived backing arrays occupy BSS. `rift`
requests a Z88DK link map for every ZXN build, validates those exported symbols
and the requested minimum, and rejects any image whose BSS enters protected
high memory. Intermediates live in a private `/tmp/rift-build-*` workspace.
`--debug` reports and retains that workspace; normal builds remove it after the
check.

The successful-build status line includes exact byte sizes. Native builds
report the executable size. ZXN builds report the final `.nex` size first and
the `_CODE.bin` program payload as the pre-wrap executable size; with
`-create-app`, Z88DK's nominal `.exe` is only a zero-byte marker.

The compiler's asset-generation component validates referenced
`SpritePattern.load` bindings and assigns their hardware pattern bases. A 4bpp
frame is 128 bytes and an odd final frame gets a zero-filled partner; an 8bpp
frame is 256 bytes. Both consume the same 256-byte physical slots and share the
hardware limit of 64.

Host builds embed the referenced bytes and format-specific upload calls in the
generated C. ZXN builds emit one temporary asset assembly file containing the
referenced bytes in exact-length PAGE sections and one coalesced startup upload
call in generated C. No `.assets` protocol, asset report, generated header,
page binary, external asset processor, or Perl invocation exists in the build.
Normal builds remove the workspace after linking; `--debug` retains it. The
banked sprite uploader is linked only when that assembly exists and is nonempty.

After Z88DK links a referenced ZXN build, the native
`build/verify-zxn-assets` utility checks PAGE_24/25 span symbols and origins,
section capacity, the selected RAM profile, NEX bank presence/count, and payload
structure. It does not reopen source assets. Byte fidelity is covered by the
compiler and emulator integration tests. The target stores no per-asset format,
frame count, path, identity, descriptor, or verifier state.

Sprite assets provisionally occupy `PAGE_24` and, when needed, `PAGE_25`.
Their map sizes and origins are collision-checked. This fixed assignment is a
temporary restriction until the shared physical-page manifest is implemented;
it must not be treated as a general allocator for other banked components.

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
tools/test-zxn test/array_test.rift
```

Pass `--emulator-bin /path/to/zesarux` instead of setting the environment
variable, `--timeout-seconds N` for slower debugging sessions, and
`--keep-builds` to retain the intermediate NEX files. Per-case compiler logs,
launcher logs, retained NEX copies, and failure diagnostics are stored under
`test-artifacts/zxn/` (ignored by Git). The suite requires a real emulator; it
will not report success merely because ZEsarUX is unavailable.
