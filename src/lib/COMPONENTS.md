# Runtime component manifest

`components.manifest` is the single compiler/driver contract for optional Rock
runtime code. The driver passes its absolute path to `rockc`; a direct `rockc`
invocation defaults to `src/lib/components.manifest` relative to the compiler
executable, never relative to the caller's working directory.

Rows are pipe-separated and lists inside fields are comma-separated. Values
may not contain whitespace, shell syntax, absolute paths, or `..` path
segments. The manifest is parsed as data and is never sourced or evaluated.

```text
component|id|dependencies|headers|host-c|zxn-c|host-asm|zxn-asm|init|shutdown|always?
builtin|rock-name|component|return-type|parameter-types|C-symbol|
lowered|rock-name|component|return-type|parameter-types|C-symbol|
opaque|Rock-type|component|constructor-name|C-symbol
method|owner|instance/type|name|component|return-type|parameter-types|C-symbol|
```

Component IDs are lowercase C identifiers. Paths are relative to `src/lib`.
`always` is the only non-empty selection flag. Headers describe public
interfaces and are always included, so an opaque handle remains usable in
parameters and aggregates without selecting its implementation. Calling an
opaque constructor or registered builtin/method selects its direct component.

The compiler resolves all bodies and deferred initializers before it writes
`OUTPUT.components`. It computes a stable manifest-order dependency closure,
emits init hooks dependency-first, and emits shutdown hooks in exact reverse
order. A normal Rock `return` from `main` jumps through that shutdown epilogue.
`halt()` and `exit()` retain their process-terminal semantics and do not promise
component shutdown.

Native standard methods borrow their receiver and explicit arguments for the
duration of the call. The compiler's ordinary scope/assignment/return rules own
the surrounding handles. Opaque cleanup and typed opaque arrays use the core
generic handle release ABI; the private object layout stays in the component C
file.

The driver validates the sidecar and uses only the target source columns for
the emitted closure. `--rtl=all` selects all manifest components as a
compatibility escape hatch for embedded C, because embedded C text cannot forge
semantic component selection.

The sidecar begins with `ROCK_COMPONENTS_V1`, then an `@profile=full`,
`core-31`, `tiny-31`, or `tiny-console-31` record, followed by component IDs.
`tiny-31`
covers empty programs and plain-ASCII literal output through the smallest
direct ULA writer. `tiny-console-31` adds Rock-owned newline, carriage-return,
tab, backspace, wrapping, and screen-scrolling behavior for escaped literal
output; it does not link z88dk's terminal. ZX Next tiny profiles reserve the
manifest component IDs `tiny_print` and `tiny_test`; `core` depends on both so
full builds remain complete. The compiler rejects a tiny build if a required
reserved component is absent. This keeps target profile/features and source
selection in compiler-produced data, with no scan of generated C symbols.

Full profiles still use `startup=1` when they need services outside this small
console contract, including formatted numeric conversion, file I/O, input, or
runtime diagnostics. Those dependencies should move independently rather than
making the lightweight console emulate the entire z88dk terminal.

`core-31` keeps the ordinary pools, strings, arrays, records, and generated
ownership code, but replaces stdio-backed printing and fatal diagnostics with
the Rock-owned console. It is selected only when the program has no optional
runtime component and contains no embedded code, raw `putchar`, or non-string
`printf`. Selecting a hardware/ROM component or using those escape hatches
falls back to `startup=1`. Lightweight fatal diagnostics print their static
format text without substituting values; this avoids carrying a general
varargs formatter solely for failure paths.

The current `sprite` entry is a compiler/ABI bootstrap for the published
`Sprite` signature table. Its portable validation/visibility shim is not the completed ZX Next
hardware sprite component; the screen/assets/DMA/bank/palette implementation
remains governed by `plans/design-zxn-sprites.md`.
