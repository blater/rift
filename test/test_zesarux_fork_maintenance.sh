#!/usr/bin/env bash

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TOOL="$ROOT/tools/maintain-zesarux-fork"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/rock-zesarux-maintenance.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

UPSTREAM="$TMP/upstream.git"
ORIGIN="$TMP/origin.git"
SEED="$TMP/seed"
CHECKOUT="$TMP/zesarux"
FAKE_ROCK="$TMP/rock"

git init --bare --initial-branch=main "$UPSTREAM" >/dev/null
git init --bare --initial-branch=main "$ORIGIN" >/dev/null
git init --initial-branch=main "$SEED" >/dev/null
git -C "$SEED" config user.name "Rock test"
git -C "$SEED" config user.email "rock-test@example.invalid"
echo baseline >"$SEED/core.txt"
mkdir -p "$SEED/src"
printf 'all:\n\t@true\n' >"$SEED/src/Makefile"
printf '#!/usr/bin/env bash\nexit 0\n' >"$SEED/src/zesarux"
chmod +x "$SEED/src/zesarux"
git -C "$SEED" add core.txt src/Makefile src/zesarux
git -C "$SEED" commit -m baseline >/dev/null
git -C "$SEED" remote add upstream "$UPSTREAM"
git -C "$SEED" remote add origin "$ORIGIN"
git -C "$SEED" push upstream main >/dev/null
git -C "$SEED" push origin main >/dev/null

git clone "$ORIGIN" "$CHECKOUT" >/dev/null
git -C "$CHECKOUT" config user.name "Rock test"
git -C "$CHECKOUT" config user.email "rock-test@example.invalid"
git -C "$CHECKOUT" remote add upstream "$UPSTREAM"
mkdir -p "$FAKE_ROCK/tools"
printf '#!/usr/bin/env bash\nexit 0\n' \
  >"$FAKE_ROCK/tools/test-zesarux-zrcp"
chmod +x "$FAKE_ROCK/tools/test-zesarux-zrcp"
printf 'test-zxn:\n\t@true\n' >"$FAKE_ROCK/Makefile"
echo fork >"$CHECKOUT/README.md"
git -C "$CHECKOUT" add README.md
git -C "$CHECKOUT" commit -m "Fork README" >/dev/null
OLD_MAIN=$(git -C "$CHECKOUT" rev-parse HEAD)
git -C "$CHECKOUT" push origin main >/dev/null
git -C "$CHECKOUT" switch -c integration/zrcp-automation >/dev/null
echo patch >"$CHECKOUT/patch.txt"
git -C "$CHECKOUT" add patch.txt
git -C "$CHECKOUT" commit -m "Focused fork patch" >/dev/null
PATCH_COMMIT=$(git -C "$CHECKOUT" rev-parse HEAD)
OLD_INTEGRATION=$(git -C "$CHECKOUT" rev-parse HEAD)
git -C "$CHECKOUT" push origin integration/zrcp-automation >/dev/null

echo upstream-change >>"$SEED/core.txt"
git -C "$SEED" add core.txt
git -C "$SEED" commit -m "Advance upstream" >/dev/null
git -C "$SEED" push upstream main >/dev/null
NEW_UPSTREAM=$(git -C "$SEED" rev-parse HEAD)

ZESARUX_SOURCE="$CHECKOUT" "$TOOL" refresh >"$TMP/refresh.log"
[[ $(git -C "$CHECKOUT" branch --show-current) == integration/zrcp-automation ]]
git -C "$CHECKOUT" merge-base --is-ancestor "$NEW_UPSTREAM" main
[[ $(git -C "$CHECKOUT" rev-list --count "$NEW_UPSTREAM..main") == 1 ]]
[[ $(git -C "$CHECKOUT" diff --name-only "$NEW_UPSTREAM" main) == README.md ]]
git -C "$CHECKOUT" merge-base --is-ancestor main integration/zrcp-automation
[[ -f "$CHECKOUT/patch.txt" ]]
[[ $(<"$CHECKOUT/README.md") == fork ]]
git -C "$CHECKOUT" diff --quiet main:README.md \
  integration/zrcp-automation:README.md --
REMOTE_MAIN=$(git -C "$CHECKOUT" rev-parse origin/main)
[[ "$REMOTE_MAIN" == "$OLD_MAIN" ]]
REMOTE_INTEGRATION=$(git -C "$CHECKOUT" \
  rev-parse origin/integration/zrcp-automation)
[[ "$REMOTE_INTEGRATION" == "$OLD_INTEGRATION" ]]
[[ $(git -C "$CHECKOUT" config --get rerere.enabled) == true ]]

REBASING_PATCH=$(git -C "$CHECKOUT" log --format=%H \
  --grep='^Focused fork patch$' -1)
ZESARUX_SOURCE="$CHECKOUT" "$TOOL" contribution \
  contribution/focused "$REBASING_PATCH" >"$TMP/contribution.log"
[[ $(git -C "$CHECKOUT" branch --show-current) == contribution/focused ]]
[[ -f "$CHECKOUT/patch.txt" ]]
[[ ! -f "$CHECKOUT/README.md" ]]
git -C "$CHECKOUT" merge-base --is-ancestor upstream/main HEAD

git -C "$CHECKOUT" switch integration/zrcp-automation >/dev/null
ZESARUX_SOURCE="$CHECKOUT" "$TOOL" status >"$TMP/status.log"
grep -q 'Outstanding fork commits:' "$TMP/status.log"
grep -q 'Focused fork patch' "$TMP/status.log"
grep -q 'Fork main overlay:' "$TMP/status.log"

git -C "$CHECKOUT" tag validation-tag-already-used
if ZESARUX_SOURCE="$CHECKOUT" "$TOOL" publish \
    validation-tag-already-used >"$TMP/tag.log" 2>&1; then
  echo "FAIL: publish accepted a reused validation tag" >&2
  exit 1
fi
grep -q 'tag already exists' "$TMP/tag.log"
REMOTE_AFTER_REJECTION=$(git -C "$CHECKOUT" \
  rev-parse origin/integration/zrcp-automation)
[[ "$REMOTE_AFTER_REJECTION" == "$OLD_INTEGRATION" ]]

echo patch >"$SEED/patch.txt"
git -C "$SEED" add patch.txt
git -C "$SEED" commit -m "Accept focused fork patch" >/dev/null
git -C "$SEED" push upstream main >/dev/null
ACCEPTED_UPSTREAM=$(git -C "$SEED" rev-parse HEAD)
ZESARUX_SOURCE="$CHECKOUT" "$TOOL" refresh >"$TMP/accepted.log"
git -C "$CHECKOUT" merge-base --is-ancestor "$ACCEPTED_UPSTREAM" main
[[ $(git -C "$CHECKOUT" rev-list --count "$ACCEPTED_UPSTREAM..main") == 1 ]]
[[ $(git -C "$CHECKOUT" diff --name-only "$ACCEPTED_UPSTREAM" main) == README.md ]]
[[ -f "$CHECKOUT/patch.txt" ]]
[[ $(<"$CHECKOUT/README.md") == fork ]]
if git -C "$CHECKOUT" log --format=%s \
    main..integration/zrcp-automation | grep -q '^Focused fork patch$'; then
  echo "FAIL: refresh retained a patch accepted upstream" >&2
  exit 1
fi
git -C "$CHECKOUT" diff --quiet main:README.md \
  integration/zrcp-automation:README.md --

PUBLISHED_MAIN=$(git -C "$CHECKOUT" rev-parse main)
PUBLISHED_INTEGRATION=$(git -C "$CHECKOUT" \
  rev-parse integration/zrcp-automation)
ZESARUX_SOURCE="$CHECKOUT" ZESARUX_ROCK_ROOT="$FAKE_ROCK" \
  "$TOOL" publish validation-readme-overlay >"$TMP/publish.log"
[[ $(git -C "$CHECKOUT" rev-parse origin/main) == "$PUBLISHED_MAIN" ]]
[[ $(git -C "$CHECKOUT" rev-parse origin/integration/zrcp-automation) == \
  "$PUBLISHED_INTEGRATION" ]]
git -C "$CHECKOUT" show-ref --verify --quiet \
  refs/tags/validation-readme-overlay
git --git-dir="$ORIGIN" show-ref --verify --quiet \
  refs/tags/validation-readme-overlay

git -C "$CHECKOUT" switch main >/dev/null
echo local-only >"$CHECKOUT/local-only.txt"
git -C "$CHECKOUT" add local-only.txt
git -C "$CHECKOUT" commit -m "Accidental local main commit" >/dev/null
if ZESARUX_SOURCE="$CHECKOUT" "$TOOL" refresh \
    >"$TMP/diverged-main.log" 2>&1; then
  echo "FAIL: refresh accepted a main branch that diverged from upstream" >&2
  exit 1
fi
grep -q 'must differ from upstream/main only by README.md' \
  "$TMP/diverged-main.log"

echo dirty >"$CHECKOUT/dirty.txt"
if ZESARUX_SOURCE="$CHECKOUT" "$TOOL" refresh >"$TMP/dirty.log" 2>&1; then
  echo "FAIL: refresh accepted a dirty checkout" >&2
  exit 1
fi
grep -q 'uncommitted changes' "$TMP/dirty.log"

echo "PASS: ZEsarUX fork maintenance refreshes one patch stack safely"
echo "PASS: contribution branches are projected from current upstream"
echo "PASS: accepted upstream patches drop from the maintained stack"
echo "PASS: publish refuses to reuse an immutable validation tag"
echo "PASS: publish updates the README overlay and integration with leases"
echo "PASS: refresh preserves the shared fork README overlay"
echo "PASS: refresh rejects non-README changes on main"
echo "PASS: refresh refuses a dirty checkout and never pushes implicitly"
