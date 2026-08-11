# Repository Guidelines

## Project Structure & Module Organization
`src/` contains compiler components (lexer, parser, typechecker, generator) and `src/lib/` holds the runtime plus ZX Next interop linked into generated programs. `build/` only caches objects created by `make` and should stay out of commits. Root-level binaries include `rockc` and the `rock` driver; language notes live in `docs/`, while `test/` collects `.rkr` regression inputs that define current behavior.

## Build, Test, and Development Commands
- `make` — compiles everything under `src/` into `rockc` using `gcc -Werror -Wall -Wextra`. Run it before testing or submitting patches.
- `make clean` — removes `build/` and `rockc`.
- `./rock path/to/foo.rkr [foo.exe]` — transpiles via `rockc` then builds the executable. Pass `--target=gcc` (default) for native runs or `--target=zxn` for ZX Next binaries (requires the `zcc +zxn` toolchain).
- `./run_tests.sh [test/substring_test.rkr]` — compiles each `.rkr` with `./rock` and runs the resulting binaries; provide a path to iterate on a single test.

## Coding Style & Naming Conventions
All C sources use two-space indentation and brace-on-same-line style (see `src/parser.c`). Use `snake_case` for functions, locals, and struct fields, reserving `UPPER_SNAKE_CASE` for enums or macros. Prefer `static` helpers for module-internal behavior and include only the headers a file needs. When invoking `./rock --debug`, generated C is formatted with `clang-format`; keep manual changes similarly tidy.

## Testing Guidelines
Tests live in `test/` with descriptive names such as `array_test.rkr` or `module_decl_test.rkr` so `run_tests.sh` can auto-discover them. Each file should import `test/Assert.rkr` helpers and emit `PASS`/`FAIL` strings that the harness parses. Run the suite after touching parsing, code generation, or runtime code. Runtime additions in `src/lib/` usually need a new `.rkr` plus, when relevant, a ZX Next check via `./rock --target=zxn`.

## Commit & Pull Request Guidelines
Existing commits are short and imperative (e.g., “Fix relative path resolution”), so follow that voice and keep unrelated work split. Before opening a PR, verify `make`, `./run_tests.sh`, and representative `./rock` invocations for each target you touched. PR descriptions should summarize behavioral impact, mention docs/tests updates, link issues, and attach logs or ZX screenshots when they illustrate new output.

## Review Charter
Review is a set of focused lenses, not a fixed team structure. Keep a change owned
by one contributor; add the reviewer(s) whose invariants it affects. The author
may use every lens as a self-check, but an independent review is required for
language behavior, runtime ownership, or target-specific changes.

### Contributor and reviewer mix

| Role | Owns | Reviews for |
| --- | --- | --- |
| Language contributor | Lexer, parser, AST, typechecker, and C generation | Rock syntax/semantics remain coherent through the full pipeline |
| Runtime and target contributor | `src/lib/`, host support, and ZX Next interop | Ownership, resource limits, and equivalent supported-target behavior |
| Test and integration contributor | Test harness, driver/build flow, and cross-cutting changes | Reproducible coverage and a small, compatible integration |
| Semantics reviewer | Observable Rock language behavior | Regressions, ambiguous syntax, type/representation mismatches, and generated-C correctness |
| Runtime/target reviewer | Runtime and target contracts | Lifetime or memory errors, host/ZXN divergence, and missing target evidence |
| Maintainer reviewer | Cross-cutting direction | Unnecessary coupling, undocumented compatibility changes, and missing user-facing docs |

For a normal feature, use the owning contributor plus one reviewer with the
relevant lens. Add a maintainer reviewer when the change crosses compiler
stages, changes a public language contract, or alters the build/driver flow.

### PR review matrix

| Change | Required independent review | Minimum evidence |
| --- | --- | --- |
| Documentation or behavior-preserving local refactor | One relevant domain reviewer | Targeted build/test when code changes |
| Lexer, parser, AST, typechecker, or generator behavior | Semantics reviewer | New or updated `.rkr` regression test; `make`; focused test run |
| Runtime allocation, strings, arrays, records, or ownership | Runtime/target reviewer | Regression test covering the lifetime/overwrite/error path; host suite or focused test |
| Host/ZX Next RTL, inline assembly, or target selection | Runtime/target reviewer | Host test where applicable and `./rock --target=zxn` compile check; hardware/emulator evidence when behavior cannot be checked on host |
| New built-in or language-visible runtime API | Semantics reviewer and runtime/target reviewer | Syntax/semantic test plus supported-target checks |
| Build scripts, `rock` driver, or test harness | Test/integration contributor or maintainer reviewer | Command(s) affected, including a representative compile/run path |
| Cross-cutting language change or compatibility decision | Maintainer reviewer plus relevant specialist | Brief PR contract: affected behavior, compatibility decision, tests, and documentation/wiki impact |

Every PR should state the user-visible outcome, affected compiler/runtime
boundary, compatibility expectation, and commands run. Blocking review comments
name the violated invariant or provide a reproducer. Use `BLOCKER` for incorrect
language behavior, memory/ownership risk, broken supported target behavior, or
missing compatibility coverage; use `REQUIRED` for an unmet matrix entry.

## Wiki
This project has a persistent knowledge wiki at `wikiroot/`. It is the compiled architectural understanding of the transpiler — always prefer it over guessing or re-deriving from scratch.

### Session start
At the start of every session, run the `/wiki` skill (no arguments). This displays the wiki README and reports any pending sources in `wikiroot/new/`.

### Building context for any task
Before reading source files, run the `/wiki query <topic>` skill first to orient. Then use `src/` to verify details or fill gaps. Applies to: adding features, fixing bugs, refactoring, tracing data flow — any task requiring system understanding.

### After significant code changes
Note which wiki pages are likely stale and mention this to the user. Do not silently let the wiki drift from the code.

### Ingest
When the user drops a file in `wikiroot/new/` or asks you to process pending sources, run the `/wiki ingest` skill.

### Lint
When the user asks to health-check or audit the wiki, run the `/wiki lint` skill.

### Wiki Reference discipline
Use reference material from wikiroot/pages before guessing or inferring unfamiliar language syntax/semantics or technologies.
If reference material is insufficient, ask for more, specifying the topic and what you are trying to solve.
