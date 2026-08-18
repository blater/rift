#!/bin/bash
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/rift-asset-language.XXXXXX)"
MANIFEST="$ROOT/test/fixtures/asset_language.manifest"
trap 'rm -rf "$WORK"' EXIT

fail() {
  echo "asset language test failed: $1" >&2
  exit 1
}

compile() {
  "$ROOT/riftc" "$1" "$2" "--component-manifest=$MANIFEST"
}

compile_zxn() {
  "$ROOT/riftc" "$1" "$2" --target=zxn \
    "--component-manifest=$MANIFEST"
}

expect_failure() {
  local source="$1" expected="$2" output="$WORK/negative"
  if compile "$source" "$output" >"$WORK/error.log" 2>&1; then
    fail "$(basename "$source") compiled successfully"
  fi
  grep -qF "$expected" "$WORK/error.log" || {
    sed 's/^/  /' "$WORK/error.log" >&2
    fail "$(basename "$source") omitted diagnostic: $expected"
  }
}

expect_manifest_failure() {
  local manifest="$1" expected="$2"
  if "$ROOT/riftc" "$WORK/no_use.rift" "$WORK/bad-manifest" \
      "--component-manifest=$manifest" >"$WORK/error.log" 2>&1; then
    fail "$(basename "$manifest") was accepted"
  fi
  grep -qF "$expected" "$WORK/error.log" || {
    sed 's/^/  /' "$WORK/error.log" >&2
    fail "$(basename "$manifest") omitted diagnostic: $expected"
  }
}

dd if=/dev/zero of="$WORK/four.spr" bs=128 count=2 status=none
dd if=/dev/zero of="$WORK/eight.spr" bs=256 count=2 status=none
dd if=/dev/zero of="$WORK/unused.spr" bs=128 count=1 status=none

cat >"$WORK/entry.rift" <<'RIFT'
SpritePattern default4 := SpritePattern.load("four.spr");
SpritePattern explicit4 := SpritePattern.load("four.spr", 4);
SpritePattern eight := SpritePattern.load("eight.spr", 8);
SpritePattern unused := SpritePattern.load("unused.spr");

sub main() {
  byte dynamic_slot := 7;
  Sprite a := Sprite(dynamic_slot);
  Sprite b := Sprite(127);
  a.position(319, 255);
  a.frame(default4, 1);
  a.frame(explicit4, 0);
  b.frame(eight, 1);
  a.show();
  a.hide();
  Sprite.hideall();
}
RIFT
compile "$WORK/entry.rift" "$WORK/entry"
test ! -e "$WORK/entry.assets" || fail "compiler retained the removed asset sidecar"
test ! -e "$WORK/entry.assets.h" || fail "compiler emitted an asset header"
test ! -e "$WORK/entry.assets.asm" || fail "host compile emitted target assembly"
grep -q 'byte a = (byte)(dynamic_slot);' "$WORK/entry.c" ||
  fail "dynamic Sprite constructor did not lower to a byte value"
grep -q 'byte b = (byte)(127);' "$WORK/entry.c" ||
  fail "literal Sprite constructor did not lower to a byte value"
grep -q 'rift__im_L6_Sprite_L8_position_A2(a, 319, 255);' "$WORK/entry.c" ||
  fail "Sprite.position ABI is wrong"
grep -q 'rift__im_L6_Sprite_L6_frame4_A2(a, 0, 1);' \
  "$WORK/entry.c" || fail "4bpp Sprite.frame lowering is wrong"
grep -q 'rift__im_L6_Sprite_L6_frame8_A2(b, 2, 1);' \
  "$WORK/entry.c" || fail "8bpp Sprite.frame lowering is wrong"
grep -q 'rift__im_L6_Sprite_L4_show_A0(a);' "$WORK/entry.c" ||
  fail "Sprite.show ABI is wrong"
grep -q 'rift__im_L6_Sprite_L4_hide_A0(a);' "$WORK/entry.c" ||
  fail "Sprite.hide ABI is wrong"
grep -q 'rift__tm_L6_Sprite_L7_hideall_A0();' "$WORK/entry.c" ||
  fail "Sprite.hideall ABI is wrong"
grep -q 'sprite_pattern_upload4(0, bytes + 0, 256, 2);' "$WORK/entry.c" ||
  fail "default 4bpp host upload is wrong"
grep -q 'sprite_pattern_upload4(1, bytes + 256, 256, 2);' "$WORK/entry.c" ||
  fail "explicit 4bpp host upload is wrong"
grep -q 'sprite_pattern_upload8(2, bytes + 512, 512, 2);' "$WORK/entry.c" ||
  fail "8bpp host upload is wrong"
if grep -qE 'four\.spr|eight\.spr|default4|explicit4|RIFT_ASSET|rift_assets_init' \
    "$WORK/entry.c"; then
  fail "generated C leaked SpritePattern declarations or metadata"
fi

compile_zxn "$WORK/entry.rift" "$WORK/entry-zxn"
grep -q '^sprite_pattern_upload_banked(24, 0, 1024, 0);$' \
  "$WORK/entry-zxn.c" || fail "ZXN asset upload call is wrong"
grep -q '^SECTION PAGE_24$' "$WORK/entry-zxn.assets.asm" ||
  fail "ZXN asset assembly omitted PAGE_24"
grep -q '^_rift_assets_page_24_start:$' "$WORK/entry-zxn.assets.asm" ||
  fail "ZXN asset assembly omitted its start symbol"
grep -q '^_rift_assets_page_24_end:$' "$WORK/entry-zxn.assets.asm" ||
  fail "ZXN asset assembly omitted its end symbol"
[ "$(grep -o '\$[0-9a-f][0-9a-f]' "$WORK/entry-zxn.assets.asm" | wc -l | tr -d ' ')" -eq 1024 ] ||
  fail "ZXN asset assembly did not contain the exact packed bytes"

# A nonzero payload proves byte fidelity and ordering.  The one-frame 4bpp
# asset occupies the first half-slot, its zero partner occupies the second,
# and the 8bpp asset begins at physical pattern base 1.
dd if=/dev/zero of="$WORK/sentinel4.spr" bs=128 count=1 status=none
printf '\021' | dd of="$WORK/sentinel4.spr" bs=1 seek=0 conv=notrunc status=none
printf '\042' | dd of="$WORK/sentinel4.spr" bs=1 seek=127 conv=notrunc status=none
dd if=/dev/zero of="$WORK/sentinel8.spr" bs=256 count=1 status=none
printf '\063' | dd of="$WORK/sentinel8.spr" bs=1 seek=0 conv=notrunc status=none
printf '\104' | dd of="$WORK/sentinel8.spr" bs=1 seek=255 conv=notrunc status=none
cat >"$WORK/sentinel.rift" <<'RIFT'
SpritePattern odd := SpritePattern.load("sentinel4.spr");
SpritePattern eight := SpritePattern.load("sentinel8.spr", 8);
sub main() {
  Sprite a := Sprite(0);
  Sprite b := Sprite(1);
  a.frame(odd, 0);
  b.frame(eight, 0);
}
RIFT
compile "$WORK/sentinel.rift" "$WORK/sentinel-host"
compile_zxn "$WORK/sentinel.rift" "$WORK/sentinel-zxn"

expected_hex="$({
  od -An -v -tx1 "$WORK/sentinel4.spr"
  dd if=/dev/zero bs=128 count=1 status=none | od -An -v -tx1
  od -An -v -tx1 "$WORK/sentinel8.spr"
} | tr -d ' \n')"
host_hex="$(sed -n '/^  static const byte bytes\[\]/,/^  };/p' \
  "$WORK/sentinel-host.c" | grep -o '0x[0-9a-f][0-9a-f]' | \
  cut -c3- | tr -d '\n')"
zxn_hex="$(grep -o '\$[0-9a-f][0-9a-f]' \
  "$WORK/sentinel-zxn.assets.asm" | cut -c2- | tr -d '\n')"
[ "$host_hex" = "$expected_hex" ] ||
  fail "host generated C changed or reordered nonzero asset bytes"
[ "$zxn_hex" = "$expected_hex" ] ||
  fail "ZXN generated assembly changed or reordered nonzero asset bytes"
grep -q 'sprite_pattern_upload4(0, bytes + 0, 256, 1);' \
  "$WORK/sentinel-host.c" || fail "odd 4bpp host padding span is wrong"
grep -q 'sprite_pattern_upload8(1, bytes + 256, 256, 1);' \
  "$WORK/sentinel-host.c" || fail "8bpp host base after odd 4bpp is wrong"
grep -q 'rift__im_L6_Sprite_L6_frame4_A2(a, 0, 0);' \
  "$WORK/sentinel-host.c" || fail "odd 4bpp frame base is wrong"
grep -q 'rift__im_L6_Sprite_L6_frame8_A2(b, 1, 0);' \
  "$WORK/sentinel-host.c" || fail "8bpp frame base is wrong"
grep -q '^sprite_pattern_upload_banked(24, 0, 512, 0);$' \
  "$WORK/sentinel-zxn.c" || fail "sentinel ZXN upload span is wrong"

for bits in 4 8; do
  if [ "$bits" -eq 4 ]; then frame_bytes=128; else frame_bytes=256; fi
  for frames in 1 2 12 23; do
    asset="$WORK/count-${bits}-${frames}.spr"
    dd if=/dev/zero of="$asset" bs="$frame_bytes" count="$frames" status=none
    cat >"$WORK/count-${bits}-${frames}.rift" <<RIFT
SpritePattern pattern := SpritePattern.load("count-${bits}-${frames}.spr", $bits);
sub main() {
  Sprite sprite := Sprite(0);
  sprite.frame(pattern, 0);
}
RIFT
    compile "$WORK/count-${bits}-${frames}.rift" \
      "$WORK/count-${bits}-${frames}"
    if [ "$bits" -eq 4 ]; then
      slots=$(((frames + 1) / 2))
    else
      slots=$frames
    fi
    packed=$((slots * 256))
    grep -q "sprite_pattern_upload${bits}(0, bytes + 0, $packed, $frames);" \
      "$WORK/count-${bits}-${frames}.c" ||
      fail "$bits bpp asset with $frames frame(s) was not laid out directly"
  done
done

dd if=/dev/zero of="$WORK/fit4.spr" bs=128 count=2 status=none
dd if=/dev/zero of="$WORK/fit8.spr" bs=256 count=63 status=none
cat >"$WORK/capacity-fit.rift" <<'RIFT'
SpritePattern four := SpritePattern.load("fit4.spr");
SpritePattern eight := SpritePattern.load("fit8.spr", 8);
sub main() {
  Sprite a := Sprite(0);
  Sprite b := Sprite(1);
  a.frame(four, 0);
  b.frame(eight, 0);
}
RIFT
compile_zxn "$WORK/capacity-fit.rift" "$WORK/capacity-fit"
grep -q '^SECTION PAGE_25$' "$WORK/capacity-fit.assets.asm" ||
  fail "exact 64-slot asset plan did not cross into PAGE_25"
grep -q '^_rift_assets_page_25_end:$' "$WORK/capacity-fit.assets.asm" ||
  fail "exact 64-slot asset plan omitted PAGE_25 end"
grep -q '^sprite_pattern_upload_banked(24, 0, 16384, 0);$' \
  "$WORK/capacity-fit.c" || fail "exact 64-slot upload length is wrong"
[ "$(grep -o '\$[0-9a-f][0-9a-f]' "$WORK/capacity-fit.assets.asm" | wc -l | tr -d ' ')" -eq 16384 ] ||
  fail "exact 64-slot assembly length is wrong"

dd if=/dev/zero of="$WORK/overflow8.spr" bs=256 count=64 status=none
cat >"$WORK/capacity-overflow.rift" <<'RIFT'
SpritePattern four := SpritePattern.load("fit4.spr");
SpritePattern eight := SpritePattern.load("overflow8.spr", 8);
sub main() {
  Sprite a := Sprite(0);
  Sprite b := Sprite(1);
  a.frame(four, 0);
  b.frame(eight, 0);
}
RIFT
expect_failure "$WORK/capacity-overflow.rift" \
  "sprite pattern capacity exceeded by asset 'eight': needs 65 slots in total, maximum is 64"

dd if=/dev/zero of="$WORK/incomplete4.spr" bs=127 count=1 status=none
cat >"$WORK/incomplete4.rift" <<'RIFT'
SpritePattern pattern := SpritePattern.load("incomplete4.spr");
sub main() {
  Sprite sprite := Sprite(0);
  sprite.frame(pattern, 0);
}
RIFT
expect_failure "$WORK/incomplete4.rift" \
  "sprite4 asset 'pattern' has 127 bytes; expected a multiple of 128"

: >"$WORK/empty8.spr"
cat >"$WORK/empty8.rift" <<'RIFT'
SpritePattern pattern := SpritePattern.load("empty8.spr", 8);
sub main() {
  Sprite sprite := Sprite(0);
  sprite.frame(pattern, 0);
}
RIFT
expect_failure "$WORK/empty8.rift" "sprite8 asset 'pattern' is empty"

cat >"$WORK/value_only.rift" <<'RIFT'
sub asset() {
}

sub main() {
  byte dynamic_slot := 200;
  Sprite Sprite := Sprite(dynamic_slot);
  Sprite copy := Sprite;
  asset();
}
RIFT
compile "$WORK/value_only.rift" "$WORK/value_only"
if grep -q '^sprite$' "$WORK/value_only.components"; then
  fail "byte-only Sprite construction selected the runtime component"
fi
grep -q 'byte Sprite = (byte)(dynamic_slot);' "$WORK/value_only.c" ||
  fail "dynamic high-bit Sprite value did not remain an inline byte"
grep -q '^byte copy = Sprite;$' "$WORK/value_only.c" ||
  fail "Sprite copy did not preserve nominal Rift type while avoiding the C typedef"
if grep -q 'Sprite_new\|rift__.*Sprite' "$WORK/value_only.c"; then
  fail "byte-only Sprite construction emitted runtime code"
fi
grep -q '^asset();$' "$WORK/value_only.c" ||
  fail "removed asset keyword did not become an ordinary identifier"

cat >"$WORK/asset_type.rift" <<'RIFT'
record asset {
  byte value
}

sub main() {
  asset sprite4 := { value := 7 };
}
RIFT
compile "$WORK/asset_type.rift" "$WORK/asset_type"
grep -q 'sprite4' "$WORK/asset_type.c" ||
  fail "legacy recognizer reserved an ordinary asset-typed sprite4 variable"

mkdir "$WORK/art"
cp "$WORK/four.spr" "$WORK/art/four.spr"
cat >"$WORK/art/Art.rift" <<'RIFT'
module Art;
SpritePattern patterns := SpritePattern.load("four.spr");
RIFT
cat >"$WORK/module_entry.rift" <<'RIFT'
include "art/Art.rift"
SpritePattern local := SpritePattern.load("unused.spr");
sub main() {
  Sprite a := Sprite(0);
  Sprite b := Sprite(1);
  a.frame(Art.patterns, 0);
  b.frame(local, 0);
}
RIFT
compile "$WORK/module_entry.rift" "$WORK/module_entry"
grep -q 'rift__im_L6_Sprite_L6_frame4_A2(a, 0, 0);' \
  "$WORK/module_entry.c" ||
  fail "module-qualified SpritePattern did not lower"
grep -q 'rift__im_L6_Sprite_L6_frame4_A2(b, 1, 0);' \
  "$WORK/module_entry.c" ||
  fail "entry binding after include was captured by the included module"
if grep -qE 'typedef struct Art|Art_new|Art_make_array|__rift_release_Art' \
    "$WORK/module_entry.c"; then
  fail "asset-only module emitted runtime scaffolding"
fi

mkdir "$WORK/plain" "$WORK/qualified"
cp "$WORK/four.spr" "$WORK/plain/four.spr"
cp "$WORK/four.spr" "$WORK/qualified/four.spr"
cat >"$WORK/plain/main.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub main() {
  Sprite s := Sprite(0);
  s.frame(patterns, 0);
}
RIFT
cat >"$WORK/qualified/Assets.rift" <<'RIFT'
module Assets;
SpritePattern patterns := SpritePattern.load("four.spr");
RIFT
cat >"$WORK/qualified/main.rift" <<'RIFT'
include "Assets.rift"
sub main() {
  Sprite s := Sprite(0);
  s.frame(Assets.patterns, 0);
}
RIFT
compile "$WORK/plain/main.rift" "$WORK/plain/game"
compile "$WORK/qualified/main.rift" "$WORK/qualified/game"
cmp "$WORK/plain/game.c" "$WORK/qualified/game.c" >/dev/null ||
  fail "asset-only module changed generated resident C"

mkdir "$WORK/mixed"
cp "$WORK/four.spr" "$WORK/mixed/four.spr"
cat >"$WORK/mixed/Mixed.rift" <<'RIFT'
module Mixed;
word counter := 0;
SpritePattern patterns := SpritePattern.load("four.spr");
RIFT
cat >"$WORK/mixed/main.rift" <<'RIFT'
include "Mixed.rift"
sub main() {
  Sprite s := Sprite(0);
  s.frame(Mixed.patterns, 0);
}
RIFT
compile "$WORK/mixed/main.rift" "$WORK/mixed/game"
grep -q '^Mixed Mixed_new(void)' "$WORK/mixed/game.c" ||
  fail "module with a runtime field lost its constructor"

cat >"$WORK/no_use.rift" <<'RIFT'
SpritePattern unused := SpritePattern.load("unused.spr");
sub main() {
  Sprite s := Sprite(200);
}
RIFT
expect_failure "$WORK/no_use.rift" "sprite slot literal 200 is outside 0..127"
cat >"$WORK/fractional_frame.rift" <<'RIFT'
SpritePattern pattern := SpritePattern.load("four.spr");
sub main() {
  Sprite sprite := Sprite(0);
  sprite.frame(pattern, 0.5);
}
RIFT
expect_failure "$WORK/fractional_frame.rift" \
  "sprite frame must be an integer literal"
cat >"$WORK/no_use.rift" <<'RIFT'
SpritePattern unused := SpritePattern.load("unused.spr");
sub main() {}
RIFT
compile "$WORK/no_use.rift" "$WORK/no_use"
if grep -q 'sprite_pattern_upload\|unused' "$WORK/no_use.c"; then
  fail "unreferenced binding changed generated C"
fi
test ! -e "$WORK/no_use.assets.asm" ||
  fail "unreferenced binding emitted target assembly"
cp "$WORK/entry-zxn.assets.asm" "$WORK/no_use.assets.asm"
compile_zxn "$WORK/no_use.rift" "$WORK/no_use"
test ! -e "$WORK/no_use.assets.asm" ||
  fail "assetless target compile retained stale asset assembly"

cat >"$WORK/legacy.rift" <<'RIFT'
asset sprite4 patterns := "four.spr";
sub main() {}
RIFT
expect_failure "$WORK/legacy.rift" "'asset' declarations were removed"

cat >"$WORK/enum_asset_collision.rift" <<'RIFT'
enum Pattern { patterns }
SpritePattern patterns := SpritePattern.load("four.spr");
sub main() {}
RIFT
expect_failure "$WORK/enum_asset_collision.rift" \
  "asset 'patterns' conflicts with an enum or union value constructor"

cat >"$WORK/union_asset_collision.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
union Pattern { byte patterns, none }
sub main() {}
RIFT
expect_failure "$WORK/union_asset_collision.rift" \
  "asset 'patterns' conflicts with an enum or union value constructor"

cat >"$WORK/nested.rift" <<'RIFT'
sub main() {
  SpritePattern patterns := SpritePattern.load("four.spr");
}
RIFT
expect_failure "$WORK/nested.rift" "SpritePattern bindings are only allowed at file scope"

cat >"$WORK/wrong_loader.rift" <<'RIFT'
SpritePattern patterns := Other.load("four.spr");
sub main() {}
RIFT
expect_failure "$WORK/wrong_loader.rift" "must call SpritePattern.load"

cat >"$WORK/nonliteral_path.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load(path);
sub main() {}
RIFT
expect_failure "$WORK/nonliteral_path.rift" "path must be a string literal"

for format in 0 5 16; do
  printf '%s\n' \
    "SpritePattern patterns := SpritePattern.load(\"four.spr\", $format);" \
    'sub main() {}' >"$WORK/bad_format.rift"
  expect_failure "$WORK/bad_format.rift" "format must be literal 4 or 8"
done
cat >"$WORK/dynamic_format.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr", depth);
sub main() {}
RIFT
expect_failure "$WORK/dynamic_format.rift" "format must be literal 4 or 8"

cat >"$WORK/bad_arity.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr", 4, 8);
sub main() {}
RIFT
expect_failure "$WORK/bad_arity.rift" "expects a path and optional format"

cat >"$WORK/storage.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub consume(SpritePattern value) {}
sub main() {
  consume(patterns);
}
RIFT
expect_failure "$WORK/storage.rift" "compile-time asset category 'SpritePattern' cannot be used as runtime storage"

cat >"$WORK/non_consumer.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub consume(word value) {}
sub main() {
  consume(patterns);
}
RIFT
expect_failure "$WORK/non_consumer.rift" "may only appear directly in a registered consumer call"

cat >"$WORK/not_pattern.rift" <<'RIFT'
sub main() {
  Sprite s := Sprite(0);
  s.frame(3, 0);
}
RIFT
expect_failure "$WORK/not_pattern.rift" "Sprite.frame pattern must be a SpritePattern binding"

cat >"$WORK/unqualified_shadow.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub main() {
  Sprite s := Sprite(0);
  word patterns := 0;
  s.frame(patterns, 0);
}
RIFT
expect_failure "$WORK/unqualified_shadow.rift" "Sprite.frame pattern must be a SpritePattern binding"

cat >"$WORK/module_shadow.rift" <<'RIFT'
include "art/Art.rift"
sub main() {
  Sprite s := Sprite(0);
  word Art := 0;
  s.frame(Art.patterns, 0);
}
RIFT
expect_failure "$WORK/module_shadow.rift" "Sprite.frame pattern must be a SpritePattern binding"

cat >"$WORK/loop_shadow.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub consume(word value) {}
sub main() {
  for patterns := 0 to 1 {
    consume(patterns);
  }
}
RIFT
compile "$WORK/loop_shadow.rift" "$WORK/loop_shadow"
if grep -q 'sprite_pattern_upload' "$WORK/loop_shadow.c"; then
  fail "loop binding marked the global SpritePattern as referenced"
fi

cat >"$WORK/bad_frame4.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("four.spr");
sub main() {
  Sprite s := Sprite(0);
  s.frame(patterns, 2);
}
RIFT
expect_failure "$WORK/bad_frame4.rift" "sprite frame literal 2 is outside 0..1"
cat >"$WORK/bad_frame8.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("eight.spr", 8);
sub main() {
  Sprite s := Sprite(0);
  s.frame(patterns, 2);
}
RIFT
expect_failure "$WORK/bad_frame8.rift" "sprite frame literal 2 is outside 0..1"

cat >"$WORK/bad_x.rift" <<'RIFT'
sub main() {
  Sprite s := Sprite(0);
  s.position(320, 0);
}
RIFT
expect_failure "$WORK/bad_x.rift" "sprite x literal 320 is outside 0..319"
cat >"$WORK/bad_y.rift" <<'RIFT'
sub main() {
  Sprite s := Sprite(0);
  s.position(0, 256);
}
RIFT
expect_failure "$WORK/bad_y.rift" "sprite y literal 256 is outside 0..255"

cat >"$WORK/default_sprite.rift" <<'RIFT'
sub main() {
  Sprite s;
}
RIFT
expect_failure "$WORK/default_sprite.rift" "Sprite has no default slot; use Sprite(byteSlot)"

cat >"$WORK/constructor_arity.rift" <<'RIFT'
sub main() {
  Sprite s := Sprite();
}
RIFT
expect_failure "$WORK/constructor_arity.rift" "Sprite() expects exactly one byte slot argument"

cat >"$WORK/missing_file.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("missing.spr");
sub main() {}
RIFT
expect_failure "$WORK/missing_file.rift" "cannot resolve asset path 'missing.spr'"
cat >"$WORK/absolute.rift" <<RIFT
SpritePattern patterns := SpritePattern.load("$WORK/four.spr");
sub main() {}
RIFT
expect_failure "$WORK/absolute.rift" "path must be relative"
mkdir "$WORK/sub"
cat >"$WORK/sub/escape.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("../four.spr");
sub main() {}
RIFT
expect_failure "$WORK/sub/escape.rift" "may not contain '..'"
mkdir "$WORK/linkroot"
ln -s "$WORK/four.spr" "$WORK/linkroot/linked.spr"
cat >"$WORK/linkroot/symlink_escape.rift" <<'RIFT'
SpritePattern patterns := SpritePattern.load("linked.spr");
sub main() {}
RIFT
expect_failure "$WORK/linkroot/symlink_escape.rift" \
  "path escapes the entry source directory"

cat >"$WORK/category_record.rift" <<'RIFT'
record SpritePattern {}
sub main() {}
RIFT
expect_failure "$WORK/category_record.rift" "compile-time asset category 'SpritePattern' is sealed"
cat >"$WORK/sprite_redefine.rift" <<'RIFT'
record Sprite {}
sub main() {}
RIFT
expect_failure "$WORK/sprite_redefine.rift" "standard type namespace 'Sprite' is sealed"
cat >"$WORK/sprite_function.rift" <<'RIFT'
sub Sprite(byte slot) returns byte { return slot; }
sub main() {}
RIFT
expect_failure "$WORK/sprite_function.rift" "function name 'Sprite' is reserved by the built-in value type"

cat >"$WORK/primitive_category.manifest" <<'MANIFEST'
component|core|||||||||always
component|sprite|core||||||||
namespace|Sprite|sprite
asset|sprite4|byte|sprite
asset|sprite8|byte|sprite
method|Sprite|instance|frame|sprite|void|byte,byte|bad_frame|
MANIFEST
expect_manifest_failure "$WORK/primitive_category.manifest" \
  "asset category collides with a runtime or namespace type"

cat >"$WORK/wrong_kind.manifest" <<'MANIFEST'
component|core|||||||||always
component|sprite|core||||||||
namespace|Sprite|sprite
asset|music|SpritePattern|sprite
method|Sprite|instance|frame|sprite|void|SpritePattern,byte|bad_frame|
MANIFEST
expect_manifest_failure "$WORK/wrong_kind.manifest" \
  "sprite assets support only sprite4/sprite8 SpritePattern assets"

cat >"$WORK/missing_kind.manifest" <<'MANIFEST'
component|core|||||||||always
component|sprite|core||||||||
namespace|Sprite|sprite
asset|sprite4|SpritePattern|sprite
method|Sprite|instance|frame|sprite|void|SpritePattern,byte|bad_frame|
MANIFEST
expect_manifest_failure "$WORK/missing_kind.manifest" \
  "sprite assets require both sprite4 and sprite8 SpritePattern asset kinds"

cat >"$WORK/wrong_consumer.manifest" <<'MANIFEST'
component|core|||||||||always
component|sprite|core||||||||
namespace|Sprite|sprite
asset|sprite4|SpritePattern|sprite
asset|sprite8|SpritePattern|sprite
method|Sprite|instance|frame|sprite|void|byte,byte|bad_frame|
MANIFEST
expect_manifest_failure "$WORK/wrong_consumer.manifest" \
  "sprite assets require Sprite.frame argument 1 to be SpritePattern"

echo "asset language tests passed"
