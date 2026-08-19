# Rift — Project TODO

Living checklist of planned work. Add items here instead of scattering them across memory or PR descriptions. Mark `[x]` when shipped; leave a one-line note on the same line if the resolution is non-obvious.

## Syntax / language

- [x] **Type-first declarations everywhere.** All name+type pairs now use `type name`; `name: type` is no longer parsed in parameters, record fields, or tagged-union variants. Sub return type uses the `returns` keyword (e.g. `sub add(int a, int b) returns int { ... }`). Lexer, parser, all 27 converted `.rift` tests, and the syntax/generator/testing wiki pages were updated together. 395/395 tests pass.
- [x] **Rename tagged-union accessors:** `.tag` → `.key`, `.data` → `.value`. Generator now emits `key`/`value` in struct definitions and `__match_tmp->key` in `case` lowering; `tagged_enum_test.rift`, `wikiroot/pages/syntax/{modules-and-records,control-flow}.md`, and `glossary.md` updated. 395/395 tests pass.
- [x] **Introduce `union` keyword for tagged unions.** New `TOK_UNION` keyword + dedicated `parse_union` producing TDEF_PRO. `parse_enum` is now plain-only (named integer constants). Lookahead-disambiguation logic and the `skip_type_in_lookahead` / `enum_has_typed_variant` helpers were removed. `match_test.rift` and `tagged_enum_test.rift` converted to `union`; wiki pages (`modules-and-records`, `syntax-index`, `control-flow`, `overview`, `glossary`, `parser-overview`) updated to reflect the split.

## Compiler

- [x] **Drop noise `to_byte`/`to_word`/`to_dword`/`to_int` casts on integer literals across the test suite** — 336 calls removed; C silently narrows int → byte/word/dword on argument passing under both gcc and SDCC, so the casts were cosmetic-only. Tests still 395/395.
- [x] **`--auto-cast` compiler flag** — when set, `riftc` wraps args with explicit `(byte)`/`(word)`/`(dword)` casts when the callee parameter is narrower than the arg type. Plumbed via `rift` wrapper too. Builtins now register parameter types via `register_builtin_typed` (extended from `register_builtin`); user-defined fundefs already had types in the AST. Cast-heavy graphics builtins (draw, plot, point, fill, circle, triangle, polyline ←TODO, etc.) and several Next/keyboard/colour builtins are typed; remaining builtins fall through to no-cast since param types aren't recorded yet.
- [ ] **Type parameter info for the rest of the builtins.** Many builtins still register only their return type via `register_builtin`. To make `--auto-cast` cover them, switch each to `register_builtin_typed` and supply param types. Low priority — only matters when `--auto-cast` is in use AND the user passes int literals to those builtins.
- [ ] **Polymorphic integer literals in the typechecker.** The proper long-term fix: numeric literals should be untyped until context picks (`42` in a `byte` slot is a `byte`, no cast needed). Once Rift has a real typechecker, this becomes the right place to handle the int → byte/word/dword coercion. `--auto-cast` is a stop-gap until then.

## Memory model

- [ ] **Aggregate field aliasing leak.** Record-literal field-init (`Outer o := {i := ii, ...}`) is a raw pointer copy with no retain, and `__handle_release` does not walk a record's aggregate/string fields. Together these produce four distinct UAF/leak/double-free shapes — full breakdown plus root cause and fix sketch in [aggregate-field-aliasing-leak](wikiroot/pages/concepts/aggregate-field-aliasing-leak.md). Three pinning tests under `test/intentionalFail/aggregate_field_*_test.rift` reproduce Shapes 1, 2, and 4 today (silent data corruption via freelist reuse) and should be moved to `test/` as each lands. Requires (a) retain on aggregate/string field-init in the `record_expr` arm of `generate_vardef`, and (b) per-type field-walking destructors invoked from `__handle_release` so freeing a record releases the rc shares its fields hold. Sequencing: do before Phase H deletes the legacy `new_string` deep-copy, since strings currently dodge most of this by deep-copying on field init.

## Runtime

- [ ] **Promote vblank interrupt ownership into a shared 50 Hz service when a second consumer appears.** The clock currently enables the ROM IM1 handler and reads `FRAMES`; a future Rift-owned interrupt could also schedule animation, input sampling, audio, timers, and deferred work. Keep those policies out of `clock_ticks()` until the runtime has a concrete additional consumer and an interrupt-ownership contract.

## Conventions for this file

- New items go under a section heading. Add new sections as needed (Compiler internals, RTL, Tooling, Docs, …).
- Each item: one-line headline in **bold**, then one or two sentences of context — what to do, why, where to touch. Enough that future-you (or a fresh session) can pick it up cold.
- When a TODO has a sequencing dependency, say so explicitly ("do after X").
- Don't track in-flight session work here — use the agent's task list for that. This file is for items that survive across sessions.
