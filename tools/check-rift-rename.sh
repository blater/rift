#!/usr/bin/env bash
# Fail when a project-identity reference escaped tools/migrate-to-rift.sh.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

matches="$({
  rg -n -i --hidden \
    -g '!build/**' \
    -g '!test-artifacts/**' \
    -g '!.git/**' \
    -g '!**/.git/**' \
    -g '!.agents/**' \
    -g '!wikiroot/log.md' \
    -g '!wikiroot/raw/**' \
    -g '!tools/migrate-to-rift.sh' \
    -g '!tools/check-rift-rename.sh' \
    'rocklang|rockc|rocker|rocktest|rock_|_rock\b|\brock\b|\.rkr\b' . || true
} | grep -v '^./wikiroot/\.git/' || true)"

if [ -n "$matches" ]; then
  printf '%s\n' 'Stale Rock project references found:' >&2
  printf '%s\n' "$matches" >&2
  exit 1
fi

echo 'PASS: no stale Rock project references found.'
