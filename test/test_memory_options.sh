#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-memory-options.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

SOURCE="$ROOT/test/fixtures/zxn_size_general_string.rift"

"$ROOT/rift" --target=gcc --memory-max=16384 --memory-min=8192 \
  "$SOURCE" "$WORK/constrained" >"$WORK/constrained.log" 2>&1 ||
  fail "host build rejected purpose-level minimum/maximum hints"
"$WORK/constrained.exe" >/dev/null 2>&1 ||
  fail "host program did not execute within its total arena limit"

REAL_GCC=$(command -v gcc)
cat >"$WORK/gcc" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$RIFT_GCC_ARGS"
exec "$RIFT_REAL_GCC" "$@"
EOF
chmod +x "$WORK/gcc"

RIFT_GCC_ARGS="$WORK/auto.args" RIFT_REAL_GCC="$REAL_GCC" \
  PATH="$WORK:$PATH" "$ROOT/rift" --target=gcc --memory-max=16384 \
  "$SOURCE" "$WORK/auto-plan" >/dev/null 2>&1 ||
  fail "host auto build failed while recording its allocator plan"
grep -qx -- '-DRIFT_ZXN_NO_BUMP_POOL' "$WORK/auto.args" ||
  fail "host auto build did not honor its no-bump build plan"

RIFT_GCC_ARGS="$WORK/all.args" RIFT_REAL_GCC="$REAL_GCC" \
  PATH="$WORK:$PATH" "$ROOT/rift" --target=gcc --rtl=all \
  --memory-max=16384 "$SOURCE" "$WORK/all-plan" >/dev/null 2>&1 ||
  fail "host all-RTL build failed while recording its allocator plan"
if grep -qx -- '-DRIFT_ZXN_NO_BUMP_POOL' "$WORK/all.args"; then
  fail "host all-RTL build incorrectly disabled its required bump frontier"
fi

for unused_hint in \
  --memory-max=4096 \
  --memory-min=0 \
  --memory-reserve=0
do
  if "$ROOT/rift" --target=zxn "$unused_hint" \
      "$ROOT/test/fixtures/zxn_size_empty.rift" "$WORK/pool-free" \
      >"$WORK/pool-free.log" 2>&1; then
    fail "pool-free build accepted unused hint $unused_hint"
  fi
  grep -q 'memory bounds require a program with managed allocation' \
    "$WORK/pool-free.log" ||
    fail "pool-free rejection was not consistent for $unused_hint"
done

if "$ROOT/rift" --target=gcc --memory-reserve=0 \
    "$SOURCE" "$WORK/host-reserve" >"$WORK/host-reserve.log" 2>&1; then
  fail "host build accepted a target-address-space reserve"
fi
grep -q -- '--memory-reserve is only meaningful for --target=zxn' \
  "$WORK/host-reserve.log" || fail "host reserve rejection was not actionable"

for old_option in \
  --memory=compact \
  --memory=standard \
  --memory-profile=zxn \
  --zxn-bump-pool=1024 \
  --zxn-longlived-pool=6144
do
  if "$ROOT/rift" --target=gcc "$old_option" \
      "$SOURCE" "$WORK/removed" >"$WORK/removed.log" 2>&1; then
    fail "driver accepted removed option $old_option"
  fi
  grep -q "unknown option '$old_option'" "$WORK/removed.log" ||
    fail "driver did not reject $old_option as removed syntax"
done

for old_option in --memory-profile=zxn --force-bump-pool
do
  if "$ROOT/riftc" "$SOURCE" "$WORK/direct" --target=gcc "$old_option" \
      >"$WORK/direct.log" 2>&1; then
    fail "direct riftc accepted removed option $old_option"
  fi
  grep -q "Unknown flag.*${old_option#--}" "$WORK/direct.log" ||
    fail "direct riftc did not reject $old_option"
done

echo "PASS: automatic memory hints work without allocator-specific controls"
