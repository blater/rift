---
name: wiki
description: >-
  Operates the Rift language knowledge wiki at /Users/blater/src/rift/wikiroot/.
  Uses explicit slash commands as the formal trigger surface: /wiki, /wiki
  help, /wiki query <question>, /wiki lint, /wiki lint fix <scope>, and /wiki
  ingest [file|pending]. Also runs session-start orientation: give a one-line
  reminder of the wiki commands, check wikiroot/new/ for pending sources, and
  proactively suggest ingest when sources are waiting. Use when the user asks
  to query the wiki, lint/health-check the wiki, ingest a source, or mentions
  wikiroot, wiki pages, or pending sources.
---

# Wiki Skill

## Context

- **Wiki root:** `/Users/blater/src/rift/wikiroot/` (a standalone Git repository)
- **Schema:** `wikiroot/wiki-proposal.md` — authoritative workflow reference (read it when you need procedural detail not covered here)
- **Source of truth:** `src/` — the wiki documents the code, never contradicts it
- **Wiki is persistent and compounding** — every operation should leave it richer and more consistent than before

## Dispatch

Use explicit `/wiki ...` keywords as the canonical human/LLM contract. If the
user phrases the request naturally but the intent is unambiguous, map it to the
matching `/wiki ...` procedure and prefer the slash form in your reply.

| Trigger | Procedure |
|---|---|
| `/wiki` or `/wiki help` | Read `wikiroot/README.md`, answer help/orientation questions, and report pending sources |
| Session start | Run the startup orientation procedure below |
| `/wiki query <question>` | Load and follow [wiki-query.md](wiki-query.md) |
| `/wiki lint` | Load and follow [wiki-lint.md](wiki-lint.md) in report-only mode |
| `/wiki lint fix <scope>` | Load and follow [wiki-lint.md](wiki-lint.md) in mutating mode for the approved fix batch |
| `/wiki ingest [file|pending]` | Load and follow [wiki-ingest.md](wiki-ingest.md) |

---

## Session Start Orientation

1. Display a one-line reminder of the formal commands:
   `"Wiki commands: /wiki help, /wiki query <question>, /wiki lint, /wiki lint fix <scope>, /wiki ingest [file|pending]."`
2. Glob `wikiroot/new/` — exclude `.gitkeep`. Count results.
3. Report: `"N source(s) pending ingest in wikiroot/new/"` (or "none pending" if 0).
4. Offer: `"Run /wiki ingest to process them."` if any are pending.
5. Read or display `wikiroot/README.md` only when the user explicitly asks for help, wiki usage details, or fuller orientation.

---

## Conventions (apply across all operations)

- **Page frontmatter** (required on every file under `wikiroot/pages/`):
  ```yaml
  ---
  title: <human-readable title>
  category: concepts | lexer | parser | generator | overview | targets | testing | syntax | decisions
  tags: []
  sources: []        # raw/ filenames this page draws from
  updated: YYYY-MM-DD
  status: current | draft | stale
  ---
  ```
- **Cross-references:** `[[filename-without-extension]]` wiki-link style
- **Ubiquitous language:** bold domain terms on first use: **term**; every term must be defined in `pages/ubiquitous-language.md`
- **Log format:** append-only entries in `wikiroot/log.md`:  
  `## [YYYY-MM-DD] <type> | <title>`  
  Types: `ingest`, `query`, `lint`, `update`, `todo`
- **Mutating runs:** any approved change batch for `/wiki ingest` or `/wiki lint fix ...` must append a log entry and create one git commit for that batch
- **Scoped commit helper:** use `./.agents/skills/wiki/scripts/log_and_commit.sh` with the exact changed file list so unrelated repo changes are excluded
- **ADR naming:** `decisions/ADR-NNNN-short-title.md` (zero-padded)
- **TODOs:** inline as `> **TODO:** <description>` on the page; also appended to log.md with type `todo`
