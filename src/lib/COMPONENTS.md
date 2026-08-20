# Runtime component manifest

`components.manifest` is the single compiler/driver contract for optional Rift
runtime code. The driver passes its absolute path to `riftc`; a direct `riftc`
invocation defaults to `src/lib/components.manifest` relative to the compiler
executable, never relative to the caller's working directory.

Rows are pipe-separated and lists inside fields are comma-separated. Values
may not contain whitespace, shell syntax, absolute paths, or `..` path
segments. The manifest is parsed as data and is never sourced or evaluated.

```text
component|id|dependencies|headers|host-c|zxn-c|host-asm|zxn-asm|init|shutdown|always?|zxn-capabilities
builtin|rift-name|component|return-type|parameter-types|C-symbol|
lowered|rift-name|component|return-type|parameter-types|C-symbol|
opaque|Rift-type|component|constructor-name|C-symbol
namespace|Rift-owner|component
asset|asset-kind|compiler-type|component
method|owner|instance/type|name|component|return-type|parameter-types|C-symbol|
```

Component IDs are lowercase C identifiers. Paths are relative to `src/lib`.
`always` is the only non-empty selection flag. ZXN capabilities are the
comma-separated values `startup31` and `pools`: the former says the component
is safe with startup 31, while the latter says its closure needs the allocator.
These are independent—a pool-free component may still require startup 1.
Headers describe public
interfaces and are always included, so an opaque handle remains usable in
parameters and aggregates without selecting its implementation. Calling an
opaque constructor or registered builtin/method selects its direct component.

The compiler resolves all bodies and deferred initializers before it writes
`OUTPUT.components`. It computes a stable manifest-order dependency closure,
emits init hooks dependency-first, and emits shutdown hooks in exact reverse
order. A normal Rift `return` from `main` jumps through that shutdown epilogue.
`halt()` and `exit()` retain their process-terminal semantics and do not promise
component shutdown.

Native standard methods borrow their receiver and explicit arguments for the
duration of the call. The compiler's ordinary scope/assignment/return rules own
the surrounding handles. Opaque cleanup and typed opaque arrays use the core
generic handle release ABI; the private object layout stays in the component C
file.

A `namespace` is a sealed type-level method owner with no runtime value,
constructor, fields, or storage. An `asset` row registers one source declaration
kind and its compiler-only category; that category may appear in registered
consumer signatures but is lowered before C emission and cannot be stored at
runtime.

The driver validates the sidecar and uses only the target source columns for
the emitted closure. `--rtl=all` selects all manifest components as a
compatibility escape hatch for embedded C, because embedded C text cannot forge
semantic component selection.

The compiler's profile is authoritative. The native driver translates it into
toolchain startup and preprocessor options but does not reclassify the profile
by inspecting selected component names.

The sidecar begins with `RIFT_COMPONENTS_V2`, a required
`@pools=none|required` record, an independent `@bump=none|required` record,
then an `@profile=full`, `core-31`, `tiny-31`, or `tiny-console-31` record,
followed by component IDs. The driver maps the profile to startup/console
flags, the pool record to `RIFT_ZXN_NO_POOLS`, and an unused bump lifetime to
`RIFT_ZXN_NO_BUMP_POOL`. `--rtl=all` keeps bump support even when the compiler
reports `@bump=none`. Embedded C is
conservatively `@bump=required`, because its allocator calls are opaque to the
AST collector. V1 sidecars and incomplete V2 metadata are rejected rather than
guessing allocator requirements.
`tiny-31`
covers empty programs and plain-ASCII literal output through the smallest
direct ULA writer. `tiny-console-31` adds Rift-owned newline, carriage-return,
tab, backspace, wrapping, and screen-scrolling behavior for escaped literal
output; it does not link z88dk's terminal. ZX Next tiny profiles reserve the
manifest component IDs `tiny_print` and `tiny_test`. The compiler selects
`tiny_test` for every `--zxn-test` build, independently of tiny eligibility;
ordinary programs do not carry the emulator protocol unit. The compiler
rejects a tiny build if a required reserved component is absent. This keeps
target profile/features and source selection in compiler-produced data, with
no scan of generated C symbols.

Full profiles still use `startup=1` when they need services outside this small
console contract, including formatted numeric conversion, file I/O, input, or
runtime diagnostics. Those dependencies should move independently rather than
making the lightweight console emulate the entire z88dk terminal.

`core-31` keeps the selected managed runtime and generated ownership code, but
replaces stdio-backed printing and fatal diagnostics with
the Rift-owned console. It is selected only when the program has no optional
runtime component and contains no embedded code, raw `putchar`, or non-string
`printf`. Selecting a hardware/ROM component or using those escape hatches
falls back to `startup=1`. Lightweight fatal diagnostics print their static
format text without substituting values; this avoids carrying a general
varargs formatter solely for failure paths.

The `core` component is restricted to pools, managed strings, and termination.
Dynamic/fixed arrays, aggregate-handle retain/release, process arguments, file
I/O, and host scalar-cast helpers live in `arrays`, `handles`, `process_args`,
`file_io`, and `scalar_casts`. The compiler selects these from resolved calls
and type/lifetime use; a string-only program does not link array, argv, file,
or aggregate-handle implementations. Target-directed `float value := input()`
selects `number_parse`; ordinary string input does not. Host terminal lifecycle and termbox
ownership live in `host_ui`; its lifecycle names are compile-time no-ops on
ZXN. Raw `peek`/`poke` support lives in `asm_interop`, while the emulator test
protocol lives in `tiny_test`. Scalar-only components such as `helpers`,
`random`, `fmath`, and `sound` do not depend on `core`, so their ZXN closures
remain pool-free (subject to each component's startup capability).

The `sprite` entry exposes the compiler-known one-byte `Sprite` value and the
compiler-only `SpritePattern` category. `Sprite(byte)` lowers without a runtime
constructor; manifest instance methods receive the slot byte directly.
Host builds select `sprite_host.c` for validation and rendering inspection.
ZXN builds select `zxn/sprite.asm`, whose complete writable state is the
128-byte attribute-3 shadow required by the hardware's write-only registers.
When referenced assets generate startup work, the driver additionally selects
the stateless `zxn/sprite_upload.asm`; assetless Sprite programs do not carry
it. The Sprite component does not own the global Layer 2/Sprite control in Next
register `$15`; programs must choose their display-layer policy explicitly, and
the raw `$15` write in the emulator smoke is test setup only. Pattern names,
formats, and placement remain build-time data governed by
[`docs/sprites-and-assets.md`](../../docs/sprites-and-assets.md).
