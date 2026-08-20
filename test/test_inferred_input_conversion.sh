#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-inferred-input.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

"$ROOT/riftc" "$ROOT/test/fixtures/inferred_input_conversion.rift" \
  "$WORK/inferred-meta" \
  --target=gcc --component-manifest="$ROOT/src/lib/components.manifest"
grep -q '^number_parse$' "$WORK/inferred-meta.components" ||
  fail "numeric input did not select number_parse"

"$ROOT/rift" --target=gcc \
  "$ROOT/test/fixtures/inferred_input_conversion.rift" \
  "$WORK/inferred" >"$WORK/compile.log" 2>&1 || {
    cat "$WORK/compile.log" >&2
    fail "inferred input fixture did not compile"
  }

printf '%s\n' '-40' '+.5' '1e2' '98.6' '32' '007' |
  "$WORK/inferred.exe" >"$WORK/actual"
if ! diff -u - "$WORK/actual" <<'EOF'
-40
-40
+.5
0.5
1e2
100
98.6
98.6
32
32
007
007
EOF
then
  fail "valid inferred conversions produced unexpected output"
fi

if printf '%s\n' 'not-a-number' | "$WORK/inferred.exe" \
    >"$WORK/invalid.log" 2>&1; then
  fail "invalid numeric input was accepted"
fi
grep -q '^Invalid numeric input$' "$WORK/invalid.log" ||
  fail "invalid numeric input did not report the runtime error"

if printf '%s\n' '1e1000' | "$WORK/inferred.exe" \
    >"$WORK/overflow.log" 2>&1; then
  fail "overflowing numeric input was accepted"
fi
grep -q '^Invalid numeric input$' "$WORK/overflow.log" ||
  fail "overflow did not report the runtime error"

"$ROOT/riftc" "$ROOT/test/fixtures/string_input_only.rift" \
  "$WORK/string-only" \
  --target=gcc --component-manifest="$ROOT/src/lib/components.manifest"
if grep -q '^number_parse$' "$WORK/string-only.components"; then
  fail "string input unnecessarily selected number_parse"
fi

echo "PASS: inferred checked input conversion"
