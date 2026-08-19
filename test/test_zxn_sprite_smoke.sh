#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rift-zxn-sprite.XXXXXX")
EMULATOR=${ZESARUX_BIN:-/Users/blater/src/zesarux/src/zesarux}
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

perl -e '
  use strict; use warnings;
  my ($four, $eight) = @ARGV;
  open my $fh4, ">:raw", $four or die "$four: $!\n";
  print {$fh4} pack("C*", map { $_ & 255 } 0 .. 255);
  close $fh4 or die "$four: $!\n";
  open my $fh8, ">:raw", $eight or die "$eight: $!\n";
  print {$fh8} pack("C*", map { (255 - $_) & 255 } 0 .. 255);
  close $fh8 or die "$eight: $!\n";
' "$WORK/four.spr" "$WORK/eight.spr"

printf '%s\n' \
  'SpritePattern four := SpritePattern.load("four.spr");' \
  'SpritePattern eight := SpritePattern.load("eight.spr", 8);' \
  'sub main() {' \
  '  Sprite hidden := Sprite(0);' \
  '  hidden.position(32, 32);' \
  '  hidden.frame(four, 1);' \
  '  hidden.show();' \
  '  hidden.hide();' \
  '  Sprite boundary := Sprite(127);' \
  '  boundary.position(319, 255);' \
  '  boundary.frame(eight, 0);' \
  '  boundary.show();' \
  '  Sprite.hideall();' \
  '  boundary.show();' \
  '  Sprite colour := Sprite(1);' \
  '  colour.position(128, 64);' \
  '  colour.frame(eight, 0);' \
  '  colour.show();' \
  '  Sprite left := Sprite(2);' \
  '  left.frame(four, 0);' \
  '  left.show();' \
  '  left.position(64, 64);' \
  '  Sprite right := Sprite(3);' \
  '  right.position(96, 64);' \
  '  right.frame(four, 0);' \
  '  right.show();' \
  '  right.frame(four, 1);' \
  '  byte invalid_slot := 128;' \
  '  Sprite invalid := Sprite(invalid_slot);' \
  '  invalid.position(1, 1);' \
  '  invalid.frame(four, 0);' \
  '  invalid.frame(eight, 0);' \
  '  invalid.show();' \
  '  invalid.hide();' \
  '  next_reg_set(21, 1);' \
  '  byte running := 1;' \
  '  while running {' \
  '  }' \
  '}' \
  >"$WORK/main.rift"

"$ROOT/rift" --target=zxn "$WORK/main.rift" "$WORK/sprite.exe"

if "$ROOT/tools/rift-emu" inspect "$WORK/sprite.nex" \
  --target zxn \
  --emulator-bin "$EMULATOR" \
  --require-emulator \
  --timeout-seconds 1 \
  --artifacts "$WORK/artifacts" >"$WORK/emulator.log" 2>&1; then
  echo 'FAIL: sprite inspection program unexpectedly returned' >&2
  exit 1
fi

if ! grep -q '"status": "timeout"' "$WORK/emulator.log"; then
  cat "$WORK/emulator.log" >&2
  echo 'FAIL: sprite inspection did not reach the expected capture timeout' >&2
  exit 1
fi
CAPTURE_DIR=$(printf '%s\n' "$WORK"/artifacts/sprite-*)

perl -e '
  use strict; use warnings;
  my $path = shift;
  open my $fh, "<", $path or die "$path: $!\n";
  my @rows;
  while (my $line = <$fh>) {
    $line =~ s/^\s+|\s+$//g;
    push @rows, $line if $line =~ /\A(?:[0-9A-F]{2} ){3,4}[0-9A-F]{2}\z/;
  }
  die "expected four sprite rows, got " . scalar(@rows) . "\n"
    unless @rows == 4;
  die "hide did not retain the selected 4bpp half: $rows[0]\n"
    unless $rows[0] eq "20 20 00 40 C0";
  die "8bpp attributes wrong: $rows[1]\n"
    unless $rows[1] eq "80 40 00 C1 00";
  die "4bpp frame 0 attributes wrong: $rows[2]\n"
    unless $rows[2] eq "40 40 00 C0 80";
  die "4bpp frame 1 attributes wrong: $rows[3]\n"
    unless $rows[3] eq "60 40 00 C0 C0";
' "$CAPTURE_DIR/sprite-attributes-0-3.txt"

grep -q '^3F FF 01 C1 00' "$CAPTURE_DIR/sprite-attributes-127.txt"

perl -e '
  use strict; use warnings;
  my $path = shift;
  open my $fh, "<", $path or die "$path: $!\n";
  local $/; my $text = <$fh>;
  my @bytes = map { hex($_) } ($text =~ /\b([0-9A-F]{2})\b/g);
  die "expected 128 pattern bytes, got " . scalar(@bytes) . "\n"
    unless @bytes == 128;
  for my $index (0 .. $#bytes) {
    die "pattern byte $index is $bytes[$index]\n"
      unless $bytes[$index] == $index;
  }
' "$CAPTURE_DIR/sprite-pattern-0-4bpp.txt"

test -s "$CAPTURE_DIR/screen.bmp"
SCREEN_COLOURS=$(perl -e '
  use strict; use warnings;
  my $path = shift;
  open my $fh, "<:raw", $path or die "$path: $!\n";
  local $/; my $bmp = <$fh>;
  die "short rendered BMP\n" unless length($bmp) >= 54;
  die "invalid rendered BMP signature\n" unless substr($bmp, 0, 2) eq "BM";
  my ($size, $offset, $width, $height, $planes, $bits) =
    (unpack("V", substr($bmp, 2, 4)), unpack("V", substr($bmp, 10, 4)),
     unpack("V", substr($bmp, 18, 4)), unpack("V", substr($bmp, 22, 4)),
     unpack("v", substr($bmp, 26, 2)), unpack("v", substr($bmp, 28, 2)));
  die "incomplete rendered BMP\n" unless length($bmp) >= $size;
  die "unexpected rendered BMP layout\n"
    unless $offset == 54 && $width == 704 && $height == 608 && $planes == 1 && $bits == 24;
  my %colours;
  for (my $at = $offset; $at + 2 < $size; $at += 3) {
    $colours{substr($bmp, $at, 3)} = 1;
  }
  print scalar(keys %colours);
' "$CAPTURE_DIR/screen.bmp")
if [ "$SCREEN_COLOURS" -lt 3 ]; then
  echo "FAIL: rendered capture has only $SCREEN_COLOURS colours" >&2
  exit 1
fi

echo "PASS: pinned ZEsarUX verified mixed 4/8bpp attributes, retained frame state, boundary slot, and rendered sprite halves ($SCREEN_COLOURS colours)"
