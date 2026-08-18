#!/usr/bin/env bash
# Rename the Rock project identity to Rift across source, tests, and docs.
#
# Run from any directory:
#   tools/migrate-to-rift.sh          # report planned changes
#   tools/migrate-to-rift.sh --apply  # rewrite text and rename affected files
#
# Generated output (build/) and Git internals are deliberately excluded.
set -euo pipefail

usage() {
  printf 'usage: %s [--apply]\n' "$(basename "$0")"
}

apply=0
case "${1:-}" in
  '') ;;
  --apply) apply=1 ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# Keep the case variants explicit: project identifiers must be renamed before
# the shorter product name, and word boundaries avoid unrelated prose such as
# "bedrock".
rewrite_stream() {
  perl -pe '
    s/\.rkr\b/.rift/g;
    s/ROCKTEST/RIFTTEST/g;
    s/Rocktest/Rifttest/g;
    s/rocktest/rifttest/g;
    s/ROCKER/RIFT/g;
    s/Rocker/Rift/g;
    s/rocker/rift/g;
    s/ROCKLANG/RIFT/g;
    s/Rocklang/Rift/g;
    s/rocklang/rift/g;
    s/ROCKC/RIFTC/g;
    s/Rockc/Riftc/g;
    s/rockc/riftc/g;
    s/ROCK_/RIFT_/g;
    s/Rock_/Rift_/g;
    s/rock_/rift_/g;
    s/_ROCK\b/_RIFT/g;
    s/_Rock\b/_Rift/g;
    s/_rock\b/_rift/g;
    s/\bROCK\b/RIFT/g;
    s/\bRock\b/Rift/g;
    s/\brock\b/rift/g;
  '
}

changed=0
while IFS= read -r -d '' path; do
  # Skip binary files and this migration/checking machinery, whose patterns
  # intentionally mention the old name.
  case "$path" in
    ./build/*|./test-artifacts/*|*/.git/*|./.agents/*|./wikiroot/log.md|./wikiroot/raw/*|./tools/migrate-to-rift.sh|./tools/check-rift-rename.sh)
      continue
      ;;
  esac
  grep -Iq . "$path" || continue
  if ! rewrite_stream < "$path" | cmp -s "$path" -; then
    printf '%s %s\n' "$([ "$apply" -eq 1 ] && printf 'rewrite' || printf 'would rewrite')" "${path#./}"
    changed=1
    if [ "$apply" -eq 1 ]; then
      temporary="${path}.rift-rename.$$"
      cp -p "$path" "$temporary"
      rewrite_stream < "$path" > "$temporary"
      mv -- "$temporary" "$path"
    fi
  fi
done < <(find . -path '*/.git' -prune -o -path './build' -prune -o -path './test-artifacts' -prune -o -type f -print0)

# Paths may also carry the product identity (the public driver and emulator
# launcher do today).  Process deepest paths first so a future directory rename
# remains safe too.  Use ordinary mv: Git recognises these as renames when the
# migration is staged, while this also works in read-only-index environments.
while IFS= read -r -d '' path; do
  case "$path" in
    ./build/*|./test-artifacts/*|*/.git/*|./.agents/*|./wikiroot/log.md|./wikiroot/raw/*|./tools/migrate-to-rift.sh|./tools/check-rift-rename.sh)
      continue
      ;;
  esac
  grep -Iq . "$path" || continue
  destination="$(printf '%s' "$path" | rewrite_stream)"
  [ "$path" = "$destination" ] && continue
  printf '%s %s -> %s\n' "$([ "$apply" -eq 1 ] && printf 'rename' || printf 'would rename')" "${path#./}" "${destination#./}"
  changed=1
  if [ "$apply" -eq 1 ]; then
    mv -- "$path" "$destination"
  fi
done < <(find . -depth -path '*/.git' -prune -o -path './build' -prune -o -path './test-artifacts' -prune -o -type f -print0)

if [ "$changed" -eq 0 ]; then
  echo 'No Rift migration changes needed.'
elif [ "$apply" -eq 0 ]; then
  echo 'Dry run only. Re-run with --apply to make these changes.'
fi
