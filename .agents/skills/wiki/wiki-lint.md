# Wiki Lint Procedure

`/wiki lint` is report-only by default. `/wiki lint fix <scope>` applies an approved batch of fixes, then logs and commits that batch.

## Steps

### 1. Run the automated lint tool

```bash
python3 wikiroot/tools/lint.py --no-colour
```

This checks all structural properties mechanically and is authoritative for:

| Check | What it catches |
|-------|----------------|
| **Frontmatter** | Missing required fields; invalid `status` values; malformed blocks |
| **Links** | Every `[[path]]` wiki-link resolved to an actual file; reports `file:lineno` |
| **Orphans** | Pages with zero inbound links from any page or `index.md` |
| **Index coverage** | Pages missing from `index.md`; `index.md` entries with no matching file |
| **Directory sizes** | Any category directory over 15 pages |
| **Pending sources** | Files/directories in `wikiroot/new/` awaiting ingest |

Capture the full output — it is included verbatim in the log body for fix runs.

Exit 0 = structurally clean. Exit 1 = issues to report.

### 2. Manual inventory

- Grep `wikiroot/log.md` for `todo` entries (pattern: `^## \[.*\] todo`)
- Grep all pages for inline `> **TODO:**` blocks

### 3. Content Health Checks

For each page under `pages/`:

- [ ] **Contradictions** — does any claim conflict with another page or with `src/`? (spot-check by topic; focus on pages sharing the same `tags` or `sources`)
- [ ] **Stale** — `updated` date significantly older than recent ingests on the same topic (`status: stale` or not yet marked stale)
- [ ] **Drafts ready to promote** — `status: draft` pages that look complete enough for `status: current`
- [ ] **Missing cross-references** — pages that clearly relate but don't link to each other (the automated tool catches broken/missing links; this checks for *absent but desirable* links)
- [ ] **Undefined terms** — bold terms (**term**) used in pages but absent from `ubiquitous-language.md`
- [ ] **Missing concept pages** — important concepts mentioned across multiple pages but lacking a dedicated page
- [ ] **Outstanding TODOs** — collect all `> **TODO:**` blocks and log.md `todo` entries found above

### 4. Structural Health Checks (manual, informed by tool output)

- [ ] **Miscategorised pages** — pages clearly in the wrong directory
- [ ] **Missing categories** — a cluster of related pages with no dedicated directory
- [ ] **Redundant categories** — directories with only 1–2 pages that would sit better elsewhere

### 5. Output

Produce a prioritised issue list grouped by type:

```
## Automated Tool
[tool exit code and summary line]

## Content Issues
P1: [Critical — contradictions, wrong claims]
P2: [Important — stale, orphans, undefined terms]
P3: [Nice-to-have — missing cross-refs, draft promotion]

## Structural Issues
[Proposed changes with rationale]

## Outstanding TODOs
[List from log.md and inline TODO blocks]

## Pending Sources
[Files in new/ awaiting ingest]

## Suggested next steps
[New questions to investigate, sources to seek]
```

### 6. Applying an Approved Fix Batch

- Do not change files during plain `/wiki lint`; report only
- Apply fixes only when the user explicitly invokes `/wiki lint fix <scope>` — that invocation is the approval
- Keep each batch focused and small enough to describe clearly in one log entry and one commit
- After updating the approved files, run the commit helper with the exact changed file list:

```sh
./.agents/skills/wiki/scripts/log_and_commit.sh lint "<brief summary>" \
  pages/<page>.md \
  index.md <<'EOF'
Automated tool: <exit code and summary>
Issues fixed: <what changed>
Remaining issues: <what was deferred, or "none">
TODOs filed: <count and short description, or "none">
EOF
```

When `wikiroot/` is a standalone repository, paths are relative to that
repository. The helper appends its `log.md` entry and creates one commit for
that fix batch. It retains the legacy project-relative behavior when the wiki
is still tracked by the project repository.
