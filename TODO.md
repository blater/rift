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

### Elastic managed memory: delivered baseline

The following checkpoints are committed and form the baseline for the remaining
work. They are not, by themselves, the completed production memory model.

- [x] **Automatic arena and non-moving segregated heap.** Commits `ef412af`,
  `ff90903`, and `0c05858` removed fixed backing pools, made managed capacity use
  the available arena, added segregated free bins, immediate coalescing, and tail
  contraction. Scalar and static-only closures can still omit the allocator.
  Evidence: [arena.h](src/lib/arena.h),
  [segregated_heap.c](src/lib/segregated_heap.c),
  [managed allocator tests](test/managed_allocator_test.c), and
  [ZXN elastic-memory tests](test/test_zxn_elastic_memory.sh).
- [x] **Verified semantic-plan foundation.** Commits `d7015cf` through
  `01b6072` added typed semantic IR, a sealed ownership plan, plan-only C
  emission, verified string calls/intrinsics, branches, loops, boolean match,
  and a bounded dynamic `string[]` subset. This remains an internal selected
  subset, not the default whole-language lowering. Evidence:
  [semantic-plan integration](test/test_semantic_plan.sh),
  [semantic IR tests](test/semantic_ir_test.c),
  [ownership-plan tests](test/ownership_plan_test.c), and
  [plan-emitter tests](test/plan_c_emitter_test.c).
- [x] **Stable managed-reference checkpoint.** Commit `26416c1` added static
  and dynamic stable references, adaptive-radix identity lookup, typed final
  destruction, scoped access and pin tokens, rollback-safe allocation, and a
  static-only no-heap path. Generated strings, arrays, and aggregates do not
  select it yet. Evidence: [managed_ref.h](src/lib/managed_ref.h),
  [managed_heap.c](src/lib/managed_heap.c),
  [managed heap tests](test/managed_heap_test.c), and
  [ZXN managed-handle tests](test/test_zxn_managed_handles.sh).
- [x] **Private routine-compaction oracle.** Commit `56f57c1` records a
  host-only, production-unselected routine mover covering cursor epochs, pin
  barriers, identity/page repair, and tail contraction. Production and ZXN
  movement remain disabled. Evidence:
  [prototype contract and rejected target measurements](test/managed_compactor_prototype.md)
  and [host compactor tests](test/managed_compactor_test.c).
- [x] **Raw aggregate aliasing repair.** Borrowed aggregate fields are retained,
  producers transfer ownership, and typed field-walking destructors now release
  nested fields. The former reproducers pass and should be moved out of
  `test/intentionalFail/`: [alias return](test/intentionalFail/aggregate_field_alias_return_test.rift),
  [source reassignment](test/intentionalFail/aggregate_field_alias_source_reassign_test.rift),
  and [overwrite/double-free](test/intentionalFail/aggregate_field_overwrite_double_free_test.rift).
  This is a correct non-moving baseline; stable aggregate migration is still
  required below.

The authoritative architecture remains
[ADR-0004](wikiroot/pages/decisions/ADR-0004-elastic-managed-memory.md). It stays
draft until the remaining sections below are completed and the legacy raw ABI
is removed.

### 1. Make semantic IR and the ownership plan total

- [ ] **Cover the complete resolved Rift program with primitive, typed IR.**

  **Still required:** Support `main`, void fallthrough, methods and receivers,
  all scalar operators/literals, short-circuit control flow, range and `for-in`
  loops, every match form, fixed and dynamic arrays of every element type,
  records, unions, modules, managed opaque/external values, globals and deferred
  initialisers, constructors/destructors, native calls, embeds, terminal calls,
  component wrappers, lifecycle, and shutdown. Generalise signatures beyond the
  current owned-string result and string/bool/`string[]` parameters. Add the
  missing plan operations for stable places, partial builders, iteration locks,
  access/pin tokens, safepoints, external linear tokens, explicit unwind,
  termination, and verified joins.

  **Tried and rejected:** Partial per-expression adoption with fallback to the
  legacy emitter is not acceptable. Unsupported selected programs deliberately
  fail before writing C; the zero-output assertions in
  [test_semantic_plan.sh](test/test_semantic_plan.sh) are the evidence. Opaque
  expression or hidden-call nodes exist only as verifier-negative cases, not as
  an escape hatch. Methods, native calls, unsupported arrays, and unknown calls
  currently fail closed rather than guessing ownership/effects.

  **Evidence:** [lower.c](src/semantic_ir/lower.c),
  [semantic_plan.c](src/generator/semantic_plan.c),
  [ownership_plan.h](src/ownership_plan/ownership_plan.h),
  [plan_c_emitter.c](src/plan_c_emitter/plan_c_emitter.c), and
  [supported dynamic string-array fixture](test/fixtures/semantic_plan_string_arrays.rift).

  **Best next step:** Extend the signature/type registry first, then land
  complete vertical slices in this order: scalars/operators plus `main`/void;
  user calls and typed core intrinsics; short-circuit/range/iteration; stable
  deep places; builders/aggregates. Native/external/lifecycle calls must wait
  for the complete manifest effect schema in section 5. Preserve whole-program
  preflight and add one invalid-plan unit test plus one host/SDCC Rift fixture
  for each slice.

### 2. Make the verified plan the sole ownership authority

- [ ] **Remove legacy AST ownership decisions after total-plan parity.**

  **Still required:** Route every function body, global initialiser,
  constructor/destructor, native wrapper, component lifecycle hook, and normal
  shutdown through the sealed plan. Add verifier rules for deep-place owner
  provenance, accesses and pins across safepoints, partial builders, external
  tokens, iteration locks, terminal wholesale teardown, and reverse successful
  global initialisation. Delete `rhs_is_borrower`, legacy temporary/scope lists,
  generator-emitted retains/releases, selected/legacy call adapters, raw block
  destructors, bump save/restore, and producer/borrower AST branches only after
  equivalent plan tests pass.

  **Tried and rejected:** The legacy full-expression ownership work fixed real
  leaks and remains valuable regression evidence, but it cannot be the final
  authority because ownership is re-inferred independently in generator cases.
  The mixed selected/legacy adapter is a migration device only. Movement before
  this cutover was rejected because raw payload addresses can still span
  effectful operations. Evidence:
  [full-expression ownership regression](test/full_expression_ownership_test.rift),
  [legacy ownership module](src/generator/ownership.c), and
  [legacy type inference](src/generator/type_info.c).

  **Best next step:** Complete whole-program plan parity, including annotated
  native/lifecycle calls, then make the plan the sole ownership authority.
  Generated-C structural tests must prove the plan emitter contains no AST
  ownership synthesis before deleting legacy ownership paths. Add the stable
  access/place/pin/safepoint seam immediately afterward with movement disabled.

### 3. Add the mandatory build contract and fingerprints

- [ ] **Replace the component sidecar with `RIFT_BUILD_CONTRACT_V1`.**

  **Still required:** Create `src/build_contract/` with a bounded canonical
  model, atomic writer, strict reader, compiler/source/assets/manifest/native
  effects/runtime ABI and target-ABI fingerprints, sorted component closure, target/profile/
  startup/memory/movement/stack fields, 256-byte line and 16-KiB total bounds,
  and exact rejection of duplicate, unknown, missing, reordered, partial, or
  overlong records. Hash the root plus transitive includes/embed inputs in
  compiler order. Share asset fingerprinting with the driver, require the
  driver to recompute it immediately before packaging and again from the bytes
  actually packaged, and add a fingerprinted runtime link symbol and
  native-wrapper namespace.

  **Tried and rejected:** Legacy sidecar V1 compatibility is already rejected.
  The current V2 component sidecar is atomically published, but it is not a
  complete build contract and must not become a permanent compatibility layer.
  The driver must not duplicate compiler include resolution, infer features by
  scanning generated C, or parse an unbounded allocation BOM.

  **Evidence:** [component sidecar writer](src/generator/components.c),
  [sidecar reader](src/driver/sidecar.c),
  [sidecar tests](test/driver_sidecar_test.c), and
  [legacy V1 fixture](test/fixtures/driver_sidecar_legacy_v1.txt).

  **Best next step:** Build writer/reader malformed-fixture tests first, add a
  small shared SHA-256/fingerprint layer, then land compiler writer, driver
  reader, and runtime link-symbol check atomically. Delete V2 in that cutover;
  do not support both formats.

### 4. Add allocation BOM and executed profiling

- [ ] **Explain every allocation and measure actual consumption.**

  **Still required:** Create `src/allocation_bom/`, deterministic `c:`/`n:`/`r:`
  site IDs, checked size formulas, purpose/trigger/lifetime/multiplicity/pinning
  metadata, canonical escaping and ordering, `--explain-allocations`, and
  `--profile-allocations`. Thread site IDs through managed allocation in
  diagnostic builds and record count, current/peak payload and physical bytes,
  handle/index overhead, fragmentation, pins, moves, pauses, arena loss, and
  whole-program residents. Emit both static `.allocations` and observed
  `.allocprofile` artifacts for acceptance examples without enlarging release
  handles.

  **Tried and rejected:** Padded NEX size is not memory accounting. The current
  report correctly separates resident image, arena address space, allocator
  capture, and instantaneous SP, but it is aggregate evidence rather than a
  per-site BOM. A single report mixing possible bounds with observed peaks,
  guessed dynamic bounds, duplicated shared metadata, or driver-consumed BOM is
  rejected.

  **Evidence:** [ZXN memory report](tools/zxn-memory-report),
  [memory-report tests](test/test_zxn_memory_report.py), and
  [current size profile](wikiroot/pages/testing/zxn-size-profile.md).

  **Best next step:** Wait until total IR names every allocation. Add the static
  compiler-only site model and ordering/escaping tests first; then add optional
  runtime counters and merge them in the report tool. Start with strings,
  arrays, allocator blocks, and adaptive-radix pages.

### 5. Define native ownership/effects and external lifecycle

- [ ] **Version-break the manifest with a complete effect contract.**

  **Still required:** Describe receiver and every parameter as scalar, borrow,
  consume, managed, pinned, or external; define owned/aliased returns, mutation
  and alias sets, allocate/collect/callback/terminate/nonreturning effects,
  destructor type, exact bank-remap/MMU slots, DMA/ISR effects, external
  create/move/release/shutdown, and idempotent terminal hooks. A terminal hook
  must register before the component's first external mutation, cover partial
  initialisation with activation state, be noalloc/nocollect/nocallback, and run
  in reverse activation/dependency order. Load native entries and core intrinsics through
  one typed registry. Reject borrowed managed returns, managed out/inout,
  persistent native managed roots, invalid nonreturning pins, unbalanced
  external tokens, callbacks into the single-context heap, and movement-unsafe
  embeds.

  **Tried and rejected:** Unannotated native calls currently fail closed in the
  semantic plan rather than receiving invented effects. A convention that
  native methods merely borrow, persistent raw pointers/handles, and a claimed
  "stable-safe embed" exemption are not sufficient. Embed must explicitly
  disable movement.

  **Evidence:** [component manifest model](src/component_manifest.h),
  [manifest schema notes](src/lib/COMPONENTS.md),
  [typed intrinsic registry](src/semantic_ir/intrinsics.c), and
  [component-manifest integration](test/test_component_manifest.sh).

  **Best next step:** Introduce the full effect grammar in one manifest version.
  Annotate one scalar component and one managed pinned fixture end-to-end, then
  migrate every entry and make missing effects a compile error before admitting
  native calls to total IR.

### 6. Route component closure from verified semantics

- [ ] **Derive the minimal runtime closure from typed IR/effects/allocation sites.**

  **Still required:** Replace the second AST/name/type walk with canonical
  requirements produced by total IR. Select `managed_heap` only for actual
  dynamic managed sites, keep static references heap-free, add independent
  container/compactor/static-handle granularity, replace `@pools/@bump` with
  build-contract memory/movement fields, and emit headers required by referenced
  public representations plus only the selected implementation closure. Mixed
  scalar/graphics/string/array programs must pay one shared heap plus only their
  used container/runtime units.

  **Tried and rejected:** Monolithic core retention, fixed compact/standard
  budgets, two elastic pools, and feature-specific backing pools were rejected.
  `--rtl=all` remains an explicit full-link mode, not permission for shallow
  automatic closure. The host compactor prototype deliberately has
  no manifest route.

  **Evidence:** [component collector](src/generator/components.c),
  [runtime manifest](src/lib/components.manifest),
  [autolink tests](test/test_rtl_autolink.sh),
  [component closure tests](test/test_component_manifest.sh), and
  [prototype isolation](test/managed_compactor_prototype.md).

  **Best next step:** Make total IR emit requirements and dual-run it against
  the current collector on focused fixtures. Require identical closures before
  deleting the AST collector. Then add mixed-program code/BSS/arena assertions.

### 7. Route all payload access through stable tokens

- [ ] **Implement the compiler/runtime access seam with movement still disabled.**

  **Still required:** Add access-begin/end, pin/unpin, stable-place, and safepoint
  operations to IR and the ownership plan. Prove the strong owner remains live,
  reject release/move while an access exists, and forbid moving safepoints until
  all accesses end. Emit `managed_ref` APIs instead of raw payload expressions.
  Deep field/index assignment must hold the terminal owner, capture indices,
  evaluate the complete RHS, then reopen access immediately before commit.
  Fuse adjacent same-handle accesses only when no release, resize, allocation,
  callback, or movement can intervene. Add stable-handle and scoped-pin native
  adapters.

  **Tried and rejected:** An unrestricted `resolve()` returning a raw pointer
  was rejected because its lifetime cannot be proven across safepoints. Existing
  raw-pointer lowering is a migration baseline only; no compiler access-seam
  implementation has yet been rejected.

  **Evidence:** [stable facade](src/lib/managed_ref.h),
  [stable heap](src/lib/managed_heap.c),
  [semantic operations](src/semantic_ir/semantic_ir.h), and
  [plan operations](src/ownership_plan/ownership_plan.h).

  **Best next step:** Make this the next runtime/compiler checkpoint. Add
  `test-managed-access`, a deep effectful-LHS Rift regression, generated-C
  structural checks, host execution, and SDCC compile. Do not enable movement.

### 8. Migrate strings to stable backing references

- [ ] **Remove persistent raw string backing pointers.**

  **Still required:** Represent backing with a stable reference plus offset,
  length, and capacity; emit static-table entries for literals; access bytes
  only inside scoped tokens; reopen after allocation/relocation; preserve
  substring views, self/overlapping append, COW growth, native I/O, return
  ownership, terminating NUL, checked arithmetic, and typed destruction. Add
  rollback-safe resize/publication and remove the raw compatibility path only
  when every producer and consumer migrates.

  **Tried and rejected:** Deep-copying every record/module string field is a
  safe non-moving workaround but duplicates storage. Persistent `data` and
  `backing` pointers cannot coexist with movement. The current raw refcounted
  representation is retained only until the access seam exists.

  **Evidence:** [string representation](wikiroot/pages/concepts/string-representation.md),
  [string runtime](src/lib/fundefs.c),
  [string growth test](test/string_growth_test.rift), and
  [aggregate aliasing analysis](wikiroot/pages/concepts/aggregate-field-aliasing-leak.md).

  **Best next step:** After the access seam, migrate the complete backing ABI in
  one runtime/generator commit. Use the private host oracle to force test-only
  moves at every safe point in substring, append, self-append, return, and
  view-lifetime tests; production movement remains disabled until every
  representation has migrated.

### 9. Migrate arrays to one stable movable payload

- [ ] **Replace the raw descriptor plus separate element buffer.**

  **Still required:** Store metadata and inline elements in one managed payload;
  remove raw slot-return APIs such as `__internal_get_elem`; resolve slots only
  under access tokens; add rollback-safe checked growth; migrate scalar, string,
  array, record, union, module, and opaque element families; migrate native
  array consumers such as `polyline`; preserve producer transfer, borrower
  copy/retain, overwrite-after-RHS, pop transfer, fixed-array bounds, and typed
  element destruction.

  **Tried and rejected:** The current two-allocation descriptor/buffer works but
  costs extra metadata and exposes raw pointers. Allocate-copy-free growth is
  safer than raw `realloc`, but it is not movement-safe. Fixed capacities or
  fixed pools are not an acceptable fallback. Unsupported array forms are
  rejected, while the supported verified `string[]` subset still emits the
  legacy array runtime representation and therefore is not movement-safe yet.

  **Evidence:** [array internals](wikiroot/pages/concepts/array-internals.md),
  [array runtime](src/lib/fundefs_internal.c),
  [verified array fixture](test/fixtures/semantic_plan_string_arrays.rift), and
  [semantic-plan array tests](test/test_semantic_plan.sh).

  **Best next step:** Migrate arrays immediately after strings, beginning with
  the already-verified `string[]` ownership path and changing only its runtime
  representation/ABI before broadening to other element families.

### 10. Migrate records, unions, modules, and managed opaque payloads

- [ ] **Replace raw aggregate handles with stable references.**

  **Still required:** Construct unpublished payloads and publish only after all
  fields initialise; generate typed destructors using private destruction
  access; lower nested field/index places through stable owner holds; migrate
  module globals and managed opaque values; preserve union live-variant
  dispatch and recursive-type rejection. Move the three now-passing alias
  regressions into the normal test set.

  **Tried and rejected:** Raw copies without matching retain/destruction caused
  the documented alias/UAF/double-free shapes. Borrower retains plus typed
  field-walking destructors fixed the non-moving baseline, but raw aggregate
  addresses remain unsuitable for movement. Do not add an aggregate-specific
  raw pointer adapter.

  **Evidence:** [handle runtime](src/lib/handle_runtime.c),
  [generator aggregate lowering](src/generator/generator.c),
  [alias return test](test/intentionalFail/aggregate_field_alias_return_test.rift),
  [source reassignment test](test/intentionalFail/aggregate_field_alias_source_reassign_test.rift),
  and [overwrite test](test/intentionalFail/aggregate_field_overwrite_double_free_test.rift).

  **Best next step:** Reuse the same stable place/access mechanism after arrays;
  add partial-builder OOM and nested collecting-RHS tests rather than another
  representation-specific ownership path.

### 11. Turn the routine compactor oracle into target runtime value

- [ ] **Implement a correct bounded ZXN routine mover after representations migrate.**

  **Still required:** Keep production movement disabled until all persistent
  payload access is handle-safe. Implement a separately selected ZXN routine
  transaction matching the host oracle operation-for-operation; insert verified
  safepoints; maintain debt/epochs across every structural and pin-topology
  mutation; advance past pin/raw/over-budget barriers; measure complete scan,
  copy, adaptive repair, bins, boundaries, and tail work; add movement/pause/
  barrier counters.

  **Tried and rejected:** The C-driven ZXN prototype measured approximately
  2,227 additional code bytes, nine BSS bytes, and 57 bytes on its deepest
  stack path. A fixed-budget naked transaction completed physical movement but
  the canonical emulator still failed stable-handle relocation, so it was
  removed. Earlier
  35/31-byte stack figures were planning estimates, not evidence. Production
  and SDCC explicitly omit the prototype.

  **Evidence:** [prototype decision record](test/managed_compactor_prototype.md),
  [host oracle tests](test/managed_compactor_test.c),
  [private heap seam](src/lib/segregated_heap_internal.h), and commit `56f57c1`.

  **Best next step:** Preserve the host oracle unchanged during representation
  migration. Then implement a fresh ZXN transaction with a direct forced-move
  emulator fixture. Treat code size as a measured baseline, but keep
  correctness, bounded work, stack high-water, and actual pause time hard.

### 12. Add complete pressure and explicit compaction

- [ ] **Recover fragmentation that routine slices cannot satisfy.**

  **Still required:** Detect fragmentation versus capacity, end access scopes,
  compact complete pin-bounded intervals, move blocks larger than the routine
  budget, maintain/rebuild bins, reserve the entire adaptive-page-plus-payload
  request set, retry exactly once, and classify pin, raw/movement-disabled,
  metadata, and capacity failure. Route verified `collect;` to a complete pass
  only when justified. Measure the entire user-visible stall including scan,
  copy, repair, bin work, reservation planning, and retry. Before choosing an
  algorithm, verify two design risks with persistent fixtures: payload-only
  early stopping may false-OOM when the same transaction also needs adaptive
  pages across pin-separated intervals; and alternating minimum-size blocks may
  make a generic per-block ZXN pass exceed the accepted 250-ms pause ceiling.

  **Tried and rejected:** Repeated routine slices are insufficient because
  large blocks are barriers. Compact-on-every-free and semispace copying were
  rejected for CPU and reserved-memory cost. Current `rift_collect()` is only a
  counted no-op and cannot serve as pressure recovery.

  **Evidence:** [ADR pressure contract](wikiroot/pages/decisions/ADR-0004-elastic-managed-memory.md),
  [current collect hook](src/lib/pools.c), and
  [prototype limitations](test/managed_compactor_prototype.md).

  **Best next step:** After host-safe representation migration, implement a
  host complete pass plus an exact all-or-nothing ordered request-set
  reservation planner. For ZXN, use a separately reviewed batch algorithm that
  detaches bins once, compacts complete intervals, specialises the minimum-block
  path, and proves the full-arena adversary plus whole retry under the accepted
  pause baseline.

### 13. Add container contraction and host region return

- [ ] **Return unused capacity automatically, not just logical length.**

  **Still required:** Add nonallocating `managed_try_shrink`; array hysteresis
  after pop/remove; optional string contraction only for unique full-span
  backing; multiple host arena regions; evacuation of later regions into earlier
  ones; release wholly empty regions; and current/peak committed-versus-
  addressable accounting. Declining shrink must never fault or invalidate the
  successful logical operation.

  **Tried and rejected:** Current arrays reduce length but never capacity;
  current strings do not contract. The host currently acquires one default
  region and releases it at shutdown. Allocating or moving merely to shrink is
  rejected; tail split is optional and nonterminal.

  **Evidence:** [array internals](wikiroot/pages/concepts/array-internals.md),
  [segregated heap tail contraction](src/lib/segregated_heap.c),
  [host arena](src/lib/arena_host.c), and [pool adapter](src/lib/pools.c).

  **Best next step:** Land array tail-split contraction after stable arrays.
  Add host multi-region evacuation/release only after production moving
  compaction is correct.

### 14. Finish leak-proof shutdown and diagnostics

- [ ] **Prove zero survivors on normal shutdown and explain every leak.**

  **Still required:** Plan all globals/modules/component lifecycle; track
  partial initialisation success; release globals/modules in reverse order while
  components remain active; shut components down in reverse dependency order;
  verify zero handles, bytes, pins, accesses, and external tokens; then release
  arenas. Distinguish normal checked shutdown from process-terminal wholesale
  teardown. Release builds need aggregate survivor counts; diagnostics need
  allocation site/category/physical bytes/owner/refcount/pins. Add leaked pin,
  access lifetime, partial constructor, external token, and global shutdown
  tests. Do not allow raw heap deinit to silently clear live counters.

  **Tried and rejected:** Process exit or arena teardown as the normal leak
  policy hides ownership defects. Soak tests catch regressions but do not prove
  zero survivors. Current managed deinit reports only a generic live-count
  fault; it is not a complete diagnostic.

  **Evidence:** [managed deinit](src/lib/managed_heap.c),
  [raw heap deinit](src/lib/segregated_heap.c),
  [managed heap tests](test/managed_heap_test.c), and the shutdown contract in
  [ADR-0004](wikiroot/pages/decisions/ADR-0004-elastic-managed-memory.md).

  **Best next step:** After representation migration, first make normal reverse
  shutdown and aggregate zero-live checks correct. Add optional per-site
  diagnostic registries afterward so release builds do not pay their RAM cost.

### 15. Complete ZXN integration and final cutover

- [ ] **Enable the new memory model only after supported-target acceptance.**

  **Still required:** Select/init/shutdown stable heap and compactor from verified
  requirements; preserve heap-free scalar/literal/graphics/static-only builds;
  execute forced movement in canonical ZEsarUX; measure combined program/runtime
  stack high-water including interrupts, no-debt safepoints, routine and pressure
  pauses, code/BSS/metadata, and arena loss; cover startup 1/31, managed/none,
  static-only, constrained arenas, native pins, assets, and MMU effects. Preserve
  unchanged source hashes and the frozen acceptance baselines: Hangman must not
  exceed 20,621 resident bytes and Clock must remain 8,037 bytes with no managed
  allocator, unless an explicit reviewed decision replaces either baseline.
  Apply the ADR cycle/pause gates. Then remove the semantic-plan feature flag and
  any residual temporary adapters/guards whose owning representation or contract
  checkpoint has not already deleted them. Publish allocation BOM/profiles and
  update README/docs/wiki; mark ADR-0004 current only after all gates pass.

  **Tried and rejected:** The incomplete target mover was removed rather than
  shipped. Instantaneous SP samples are not stack high-water proof. Old pool
  flags/contracts, dual lowering, dual build contracts, fixed profiles, and
  compatibility for accidental evaluation order are explicitly rejected.
  README currently contains stale allocator-cache wording and must be corrected
  at cutover.

  **Evidence:** [ZXN size profile](wikiroot/pages/testing/zxn-size-profile.md),
  [testing overview](wikiroot/pages/testing/testing-overview.md),
  [managed-handle target tests](test/test_zxn_managed_handles.sh),
  [ZXN elastic-memory tests](test/test_zxn_elastic_memory.sh), and
  [prototype record](test/managed_compactor_prototype.md).

  **Best next step:** Do not make movement the next production switch. Follow
  the dependency order below, land and measure each representation checkpoint,
  then perform one atomic final cutover with no compatibility layer.

### Recommended delivery order

1. Total IR for `main`, scalar/control flow, user calls, and typed core
   intrinsics.
2. Version-break the native manifest with complete ownership/effects, then
   lower native/external/lifecycle operations and finish whole-program plan
   parity.
3. Make the verified plan the sole ownership/lifecycle authority and remove
   legacy AST ownership synthesis; retain only explicitly scoped ABI adapters.
4. Stable access/place/pin/safepoint seam with movement disabled.
5. Stable strings.
6. Stable arrays and native array consumers.
7. Stable aggregates, modules, unions, and opaque payloads.
8. Build contract and verified component routing; allocation BOM can then name
   complete sites.
9. Host routine/pressure movement and ordered request-set retry.
10. ZXN routine and pressure movement with measured stack/pause baselines.
11. Array/string contraction, host segment evacuation, and leak diagnostics.
12. Final target matrix, example BOM/profiles, documentation, and atomic public
    cutover.

## Runtime

- [ ] **Promote vblank interrupt ownership into a shared 50 Hz service when a second consumer appears.** The clock currently enables the ROM IM1 handler and reads `FRAMES`; a future Rift-owned interrupt could also schedule animation, input sampling, audio, timers, and deferred work. Keep those policies out of `clock_ticks()` until the runtime has a concrete additional consumer and an interrupt-ownership contract.

## Conventions for this file

- New items go under a section heading. Add new sections as needed (Compiler internals, RTL, Tooling, Docs, …).
- Ordinary items use a one-line headline in **bold**, then one or two sentences
  of context. Cross-cutting delivery plans may use the structured **Still
  required / Tried and rejected / Evidence / Best next step** format above so a
  future session does not repeat rejected work or lose acceptance evidence.
- When a TODO has a sequencing dependency, say so explicitly ("do after X").
- Don't track in-flight session work here — use the agent's task list for that. This file is for items that survive across sessions.
