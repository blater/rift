#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERIFY="$ROOT/build/verify-zxn-assets"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-assets.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

collect_debug_build() {
  log="$1"
  destination="$2"
  debug_dir=$(sed -n 's/^rift: debug workspace: //p' "$log" | head -1)
  [ -n "$debug_dir" ] && [ -d "$debug_dir" ] ||
    fail "build did not report its debug workspace: $log"
  cp "$debug_dir"/* "$destination"/
  rm -rf "$debug_dir"
}

write_map() {
  path="$1"
  page24_size="$2"
  page24_start="$3"
  page24_end="$4"
  page25_size="${5:-}"
  {
    printf '__PAGE_24_head = $c000\n'
    printf '__PAGE_24_size = $%04x\n' "$page24_size"
    printf '_rift_assets_page_24_start = $%04x\n' "$page24_start"
    printf '_rift_assets_page_24_end = $%04x\n' "$page24_end"
    if [ -n "$page25_size" ]; then
      printf '__PAGE_25_head = $e000\n'
      printf '__PAGE_25_size = $%04x\n' "$page25_size"
      printf '_rift_assets_page_25_start = $e000\n'
      printf '_rift_assets_page_25_end = $%04x\n' "$((0xe000 + page25_size))"
    fi
  } >"$path"
}

write_nex() {
  path="$1"
  ram_required="$2"
  bank_count="$3"
  bank12="$4"
  bank48="$5"
  payload_banks="$6"
  dd if=/dev/zero of="$path" bs=512 count=1 status=none
  printf 'Next' | dd of="$path" bs=1 seek=0 conv=notrunc status=none
  printf "\\$(printf '%03o' "$ram_required")" |
    dd of="$path" bs=1 seek=8 conv=notrunc status=none
  printf "\\$(printf '%03o' "$bank_count")" |
    dd of="$path" bs=1 seek=9 conv=notrunc status=none
  printf "\\$(printf '%03o' "$bank12")" |
    dd of="$path" bs=1 seek=30 conv=notrunc status=none
  printf "\\$(printf '%03o' "$bank48")" |
    dd of="$path" bs=1 seek=66 conv=notrunc status=none
  dd if=/dev/zero bs=16384 count="$payload_banks" status=none >>"$path"
}

expect_rejection() {
  expected="$1"
  shift
  if "$VERIFY" "$@" >"$WORK/reject.log" 2>&1; then
    fail "native verifier accepted invalid input: $expected"
  fi
  grep -qF "$expected" "$WORK/reject.log" || {
    sed 's/^/  /' "$WORK/reject.log" >&2
    fail "native verifier omitted diagnostic: $expected"
  }
}

write_referenced_program() {
  directory="$1"
  count="$2"
  index=0
  : >"$directory/main.rift"
  while [ "$index" -lt "$count" ]; do
    dd if=/dev/zero of="$directory/pattern-$index.spr" bs=128 count=1 \
      status=none
    printf 'SpritePattern pattern%s := SpritePattern.load("pattern-%s.spr");\n' \
      "$index" "$index" >>"$directory/main.rift"
    index=$((index + 1))
  done
  printf '%s\n' 'sub main() {' '  Sprite sprite := Sprite(0);' \
    >>"$directory/main.rift"
  index=0
  while [ "$index" -lt "$count" ]; do
    printf '  sprite.frame(pattern%s, 0);\n' "$index" \
      >>"$directory/main.rift"
    index=$((index + 1))
  done
  printf '%s\n' '}' >>"$directory/main.rift"
}

verify_startup_cost() {
  prefix="$1"
  expected_code="$2"
  sed '/^[[:space:]]*sprite_pattern_upload_banked(/d' \
    "$prefix.exe.c" >"$prefix-control.c"
  zcc +zxn -m -vn -subtype=nex -startup=31 -clib=sdcc_iy --opt-code-size \
    -create-app \
    -DRIFT_ZXN_TINY_CORE -DRIFT_ZXN_TINY_PRINT_DIRECT \
    -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
    -I"$ROOT/src/lib" -lm \
    -o "$prefix-control.exe" "$prefix-control.c" \
    "$ROOT/src/lib/zxn/sprite.asm" >"$prefix-control.log"
  target_code=$(wc -c <"${prefix}_CODE.bin" | tr -d ' ')
  control_code=$(wc -c <"$prefix-control_CODE.bin" | tr -d ' ')
  [ $((target_code - control_code)) -eq "$expected_code" ] ||
    fail "referenced asset startup changed CODE by $((target_code - control_code)) bytes, expected $expected_code"
  target_bss=$(awk '$1 == "__BSS_size" { print $3; exit }' "$prefix.map")
  control_bss=$(awk '$1 == "__BSS_size" { print $3; exit }' \
    "$prefix-control.map")
  [ "$target_bss" = "$control_bss" ] ||
    fail "referenced assets changed BSS ($control_bss to $target_bss)"
}

[ -x "$VERIFY" ] || fail "missing native verifier $VERIFY"

write_map "$WORK/one.map" 256 0xc000 0xc100
write_nex "$WORK/one.nex" 0 1 1 0 1
"$VERIFY" --map "$WORK/one.map" --nex "$WORK/one.nex" --ram-pages=96

write_map "$WORK/two.map" 8192 0xc000 0xe000 256
"$VERIFY" --map "$WORK/two.map" --nex "$WORK/one.nex" --ram-pages=96

write_map "$WORK/early25.map" 256 0xc000 0xc100 256
expect_rejection 'PAGE_25 is present before the PAGE_24 asset span is full' \
  --map "$WORK/early25.map" --nex "$WORK/one.nex"

write_map "$WORK/prefix.map" 257 0xc001 0xc101
expect_rejection 'PAGE_24 asset span begins at 0xC001, expected 0xC000' \
  --map "$WORK/prefix.map" --nex "$WORK/one.nex"

write_map "$WORK/overflow.map" 8193 0xc000 0xe001
expect_rejection 'PAGE_24 asset span is not within one 8 KiB page' \
  --map "$WORK/overflow.map" --nex "$WORK/one.nex"

sed '/_rift_assets_page_24_end/d' "$WORK/one.map" >"$WORK/incomplete.map"
expect_rejection 'link map has an incomplete PAGE_24 asset span' \
  --map "$WORK/incomplete.map" --nex "$WORK/one.nex"

cp "$WORK/one.nex" "$WORK/signature.nex"
printf 'Nope' | dd of="$WORK/signature.nex" bs=1 seek=0 conv=notrunc status=none
expect_rejection 'bad NEX signature' \
  --map "$WORK/one.map" --nex "$WORK/signature.nex"

write_nex "$WORK/profile.nex" 1 1 1 0 1
expect_rejection 'NEX RAM_Required=1, expected 0' \
  --map "$WORK/one.map" --nex "$WORK/profile.nex" --ram-pages=96
"$VERIFY" --map "$WORK/one.map" --nex "$WORK/profile.nex" --ram-pages=224

write_nex "$WORK/count.nex" 0 2 1 0 1
expect_rejection 'NEX bank count differs from its presence table' \
  --map "$WORK/one.map" --nex "$WORK/count.nex"

write_nex "$WORK/no-bank.nex" 0 0 0 0 0
expect_rejection 'NEX omits asset bank 12 for PAGE_24/25' \
  --map "$WORK/one.map" --nex "$WORK/no-bank.nex"

write_nex "$WORK/expanded-bank.nex" 0 2 1 1 2
expect_rejection 'NEX contains a bank outside the 96-page RAM profile' \
  --map "$WORK/one.map" --nex "$WORK/expanded-bank.nex"

cp "$WORK/one.nex" "$WORK/trailing.nex"
printf x >>"$WORK/trailing.nex"
expect_rejection 'NEX payload length differs from its bank table' \
  --map "$WORK/one.map" --nex "$WORK/trailing.nex"

mkdir "$WORK/link"
dd if=/dev/zero of="$WORK/link/pattern.bin" bs=256 count=1 status=none
printf 'int main(void) { return 0; }\n' >"$WORK/link/main.c"
printf '%s\n' \
  'SECTION PAGE_24' \
  'PUBLIC _rift_assets_page_24_start' \
  'PUBLIC _rift_assets_page_24_end' \
  '_rift_assets_page_24_start:' \
  "  BINARY \"$WORK/link/pattern.bin\"" \
  '_rift_assets_page_24_end:' \
  >"$WORK/link/assets.asm"
printf '%s\n' \
  'SECTION PAGE_24' \
  '_rift_asset_tail_probe:' \
  '  defb $aa' \
  >"$WORK/link/tail.asm"
zcc +zxn -m -vn -subtype=nex -startup=1 -clib=sdcc_iy -create-app \
  -pragma-include:"$ROOT/src/lib/zxn/zpragma_zxn.inc" \
  -o "$WORK/link/game.exe" "$WORK/link/main.c" "$WORK/link/assets.asm" \
  "$WORK/link/tail.asm"
"$VERIFY" --map "$WORK/link/game.map" --nex "$WORK/link/game.nex" \
  --ram-pages=96
grep -Eiq '^__PAGE_24_size[[:space:]]*=[[:space:]]*\$0101\b' \
  "$WORK/link/game.map" || fail 'real linker did not retain the reusable PAGE_24 tail'

for referenced_count in 1 2 12 23; do
  count_dir="$WORK/count-$referenced_count"
  mkdir "$count_dir"
  write_referenced_program "$count_dir" "$referenced_count"
  "$ROOT/rift" --debug --target=gcc "$count_dir/main.rift" \
    "$count_dir/game.exe" >"$count_dir/build.log"
  collect_debug_build "$count_dir/build.log" "$count_dir"
  "$count_dir/game.exe"
  actual_count=$(grep -c '  sprite_pattern_upload4(' "$count_dir/game.exe.c")
  [ "$actual_count" -eq "$referenced_count" ] ||
    fail "$referenced_count referenced assets emitted $actual_count host uploads"
  [ ! -e "$count_dir/game.exe.assets.asm" ] ||
    fail 'host build emitted ZXN asset assembly'
done

mkdir "$WORK/mixed"
dd if=/dev/zero of="$WORK/mixed/four.spr" bs=128 count=3 status=none
dd if=/dev/zero of="$WORK/mixed/eight.spr" bs=256 count=2 status=none
printf '%s\n' \
  'SpritePattern four := SpritePattern.load("four.spr");' \
  'SpritePattern eight := SpritePattern.load("eight.spr", 8);' \
  'sub main() {' \
  '  Sprite left := Sprite(0);' \
  '  Sprite right := Sprite(1);' \
  '  left.frame(four, 2);' \
  '  right.frame(eight, 1);' \
  '}' >"$WORK/mixed/main.rift"
"$ROOT/rift" --debug --target=gcc "$WORK/mixed/main.rift" \
  "$WORK/mixed/game.exe" >"$WORK/mixed/build.log"
collect_debug_build "$WORK/mixed/build.log" "$WORK/mixed"
"$WORK/mixed/game.exe"
grep -q 'sprite_pattern_upload4(0,' "$WORK/mixed/game.exe.c" ||
  fail 'mixed host build omitted its 4bpp upload'
grep -q 'sprite_pattern_upload8(2,' "$WORK/mixed/game.exe.c" ||
  fail 'mixed host build assigned the wrong 8bpp physical base'

mkdir "$WORK/capacity"
dd if=/dev/zero of="$WORK/capacity/full.spr" bs=256 count=64 status=none
dd if=/dev/zero of="$WORK/capacity/extra.spr" bs=256 count=1 status=none
printf '%s\n' \
  'SpritePattern full := SpritePattern.load("full.spr", 8);' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(full, 63);' \
  '}' >"$WORK/capacity/full.rift"
"$ROOT/riftc" "$WORK/capacity/full.rift" "$WORK/capacity/full.exe" \
  --target=zxn --component-manifest="$ROOT/src/lib/components.manifest"
grep -q '^SECTION PAGE_25$' "$WORK/capacity/full.exe.assets.asm" ||
  fail 'exact 64-slot layout did not emit PAGE_25'
printf '%s\n' \
  'SpritePattern full := SpritePattern.load("full.spr", 8);' \
  'SpritePattern extra := SpritePattern.load("extra.spr", 8);' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(full, 63);' \
  '  sprite.frame(extra, 0);' \
  '}' >"$WORK/capacity/over.rift"
if "$ROOT/riftc" "$WORK/capacity/over.rift" "$WORK/capacity/over.exe" \
    --target=zxn --component-manifest="$ROOT/src/lib/components.manifest" \
    >"$WORK/capacity/over.log" 2>&1; then
  fail 'compiler accepted 65 simultaneously resident physical patterns'
fi
grep -q 'sprite pattern capacity exceeded' "$WORK/capacity/over.log" ||
  fail '65-slot rejection omitted its capacity diagnostic'

mkdir "$WORK/cross"
dd if=/dev/zero of="$WORK/cross/four.spr" bs=128 count=1 status=none
dd if=/dev/zero of="$WORK/cross/eight.spr" bs=256 count=32 status=none
printf '%s\n' \
  'SpritePattern four := SpritePattern.load("four.spr");' \
  'SpritePattern eight := SpritePattern.load("eight.spr", 8);' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(four, 0);' \
  '  sprite.frame(eight, 31);' \
  '}' >"$WORK/cross/main.rift"
"$ROOT/rift" --debug --target=zxn "$WORK/cross/main.rift" \
  "$WORK/cross/game.exe" >"$WORK/cross/build.log"
collect_debug_build "$WORK/cross/build.log" "$WORK/cross"
[ -s "$WORK/cross/game.exe.assets.asm" ] ||
  fail '--debug did not retain generated asset assembly'
grep -q '^_rift_assets_page_25_start:$' "$WORK/cross/game.exe.assets.asm" ||
  fail 'cross-page asset did not emit a PAGE_25 span'
grep -q 'asset build verified: PAGE_24/25' "$WORK/cross/build.log" ||
  fail 'driver did not run the native verifier for a cross-page build'
verify_startup_cost "$WORK/cross/game" 111
rm -f "$WORK/cross/game.exe.assets.asm"

"$ROOT/rift" --target=zxn "$WORK/cross/main.rift" \
  "$WORK/cross/game.exe" >"$WORK/cross/normal.log"
[ ! -e "$WORK/cross/game.exe.assets.asm" ] ||
  fail 'normal build retained generated asset assembly'

mkdir "$WORK/empty"
printf '%s\n' 'sub main() {' '}' >"$WORK/empty/main.rift"
"$ROOT/rift" --debug --target=zxn "$WORK/empty/main.rift" \
  "$WORK/empty/game.exe" >"$WORK/empty/build.log"
collect_debug_build "$WORK/empty/build.log" "$WORK/empty"
[ ! -e "$WORK/empty/game.exe.assets.asm" ] ||
  fail 'assetless build emitted asset assembly'
if grep -q '_sprite_pattern_upload_banked' "$WORK/empty/game.map"; then
  fail 'assetless build linked the conditional sprite uploader'
fi

mkdir "$WORK/reach-a" "$WORK/reach-b"
dd if=/dev/zero of="$WORK/reach-a/used.spr" bs=128 count=1 status=none
cp "$WORK/reach-a/used.spr" "$WORK/reach-b/used.spr"
dd if=/dev/zero of="$WORK/reach-b/unused.spr" bs=256 count=64 status=none
printf '%s\n' \
  'SpritePattern used := SpritePattern.load("used.spr");' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(used, 0);' \
  '}' >"$WORK/reach-a/main.rift"
printf '%s\n' \
  'SpritePattern unused := SpritePattern.load("unused.spr", 8);' \
  'SpritePattern used := SpritePattern.load("used.spr");' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(used, 0);' \
  '}' >"$WORK/reach-b/main.rift"
for reach_dir in reach-a reach-b; do
  "$ROOT/rift" --debug --target=zxn "$WORK/$reach_dir/main.rift" \
    "$WORK/$reach_dir/game.exe" >"$WORK/$reach_dir/build.log"
  collect_debug_build "$WORK/$reach_dir/build.log" "$WORK/$reach_dir"
done
verify_startup_cost "$WORK/reach-a/game" 111
for artifact in game.nex game_CODE.bin game.exe.c game.exe.assets.asm; do
  cmp "$WORK/reach-a/$artifact" "$WORK/reach-b/$artifact" ||
    fail "unreferenced 64-slot declaration changed $artifact"
done

space_dir="$WORK/source and output path"
mkdir "$space_dir"
dd if=/dev/zero of="$space_dir/one pattern.spr" bs=128 count=1 status=none
printf '%s\n' \
  'SpritePattern pattern := SpritePattern.load("one pattern.spr");' \
  'sub main() {' \
  '  Sprite sprite := Sprite(0);' \
  '  sprite.frame(pattern, 0);' \
  '}' >"$space_dir/main program.rift"
"$ROOT/rift" --debug --target=zxn "$space_dir/main program.rift" \
  "$space_dir/target.exe" >"$space_dir/build.log"
collect_debug_build "$space_dir/build.log" "$space_dir"
[ -f "$space_dir/target.nex" ] ||
  fail 'referenced ZXN build failed to produce a ZXN in a spaced output path'
[ -s "$space_dir/target.exe.assets.asm" ] ||
  fail 'spaced referenced ZXN build did not retain its generated assembly'
grep -q 'asset build verified: PAGE_24' "$space_dir/build.log" ||
  fail 'spaced referenced ZXN build did not run the native verifier'

if rg -n 'pack-zxn-assets|RIFT_ASSETS_V[0-9]|assets-host|assets\.report|assets\.page(24|25)|perl[^#\n]*asset|asset[^#\n]*perl' \
    "$ROOT/rift" "$ROOT/Makefile" "$ROOT/src" "$ROOT/tools" \
    >"$WORK/perl.log"; then
  sed 's/^/  /' "$WORK/perl.log" >&2
  fail 'asset build path still invokes Perl or the obsolete packer'
fi

echo 'PASS: native verifier rejects malformed PAGE_24/25 maps and NEX structure'
echo 'PASS: native verifier accepts a real Z88DK PAGE_24 map and NEX'
echo 'PASS: direct generation covers 1, 2, 12, and 23 referenced assets plus mixed 4/8bpp layout'
echo 'PASS: exact capacity, overflow, page crossing, cleanup, debug retention, and referenced-only output hold'
echo 'PASS: one-page and mixed cross-page startup remain exactly 111 CODE bytes and 0 BSS bytes'
echo 'PASS: referenced ZXN assets build from source and output paths containing spaces'
echo 'PASS: production asset build path contains no Perl or packing-stage invocation'
