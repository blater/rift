#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d /tmp/rift-driver-locations.XXXXXX)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/source tree/includes" "$WORK/invocation/output dir"
cp "$ROOT/test/Assert.rift" "$WORK/source tree/Assert.rift"
cp "$ROOT/test/simple_test.rift" "$WORK/source tree/main.rift"

(
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc "../source tree/main.rift" "output dir/local program"
)

[ -x "$WORK/invocation/output dir/local program.exe" ] || {
  echo 'FAIL: explicit relative host output was not anchored to the invocation directory' >&2
  exit 1
}
"$WORK/invocation/output dir/local program.exe" >/dev/null
[ ! -e "$WORK/invocation/output dir/local program.exe.c" ]
[ ! -e "$WORK/invocation/output dir/local program.exe.components" ]
[ ! -e "$WORK/invocation/output dir/local program.exe.assets.asm" ]

printf 'known-good-output\n' >"$WORK/invocation/output dir/preserved.exe"
printf 'this is not valid Rift source\n' >"$WORK/source tree/invalid.rift"
if (
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc "../source tree/invalid.rift" "output dir/preserved"
) >/dev/null 2>&1; then
  echo 'FAIL: invalid Rift source compiled successfully' >&2
  exit 1
fi
test "$(cat "$WORK/invocation/output dir/preserved.exe")" = 'known-good-output'

if (
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc --debug "../source tree/invalid.rift" "output dir/debug failure"
) >"$WORK/debug-failure.log" 2>&1; then
  echo 'FAIL: invalid debug build compiled successfully' >&2
  exit 1
fi
failed_debug_dir=$(sed -n 's/^rift: debug workspace: //p' \
  "$WORK/debug-failure.log" | head -1)
[ -n "$failed_debug_dir" ] && [ -d "$failed_debug_dir" ] || {
  echo 'FAIL: failed debug build did not retain its reported workspace' >&2
  exit 1
}
rm -rf "$failed_debug_dir"

(
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc "../source tree/main.rift"
)

[ -x "$WORK/source tree/main.exe" ] || {
  echo 'FAIL: default host output was not placed beside the source' >&2
  exit 1
}

cp "$WORK/source tree/main.rift" "$WORK/source tree/alias.rft"
(
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc "../source tree/alias.rft"
)
[ -x "$WORK/source tree/alias.exe" ] || {
  echo 'FAIL: .rft source alias was not accepted or stripped from the output name' >&2
  exit 1
}

mkdir "$WORK/bin"
ln -s "$ROOT/rift" "$WORK/bin/rift"
(
  cd "$WORK/invocation"
  PATH="$WORK/bin:$PATH" rift --target=gcc "../source tree/main.rift" "output dir/path launch"
)
[ -x "$WORK/invocation/output dir/path launch.exe" ] || {
  echo 'FAIL: PATH/symlink invocation did not resolve Rift resources' >&2
  exit 1
}

debug_log="$WORK/debug.log"
(
  cd "$WORK/invocation"
  "$ROOT/rift" --target=gcc --debug "../source tree/main.rift" "output dir/debug program"
) >"$debug_log" 2>&1
debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$debug_log" | head -1)
[ -n "$debug_dir" ] && [ -d "$debug_dir" ] || {
  cat "$debug_log" >&2
  echo 'FAIL: --debug did not report and retain its workspace' >&2
  exit 1
}
[ -f "$debug_dir/debug program.exe.c" ] || {
  echo 'FAIL: debug workspace did not contain generated C' >&2
  exit 1
}
rm -rf "$debug_dir"

echo 'PASS: driver resolves .rift/.rft source, output, executable, spaced, and debug paths independently of cwd'
