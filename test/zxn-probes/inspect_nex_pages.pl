#!/usr/bin/env perl
use strict;
use warnings;

my ($path, $map_path, $profile) = @ARGV;
die "usage: $0 probe.nex probe.map [base|expanded]\n"
  unless defined $path && defined $map_path;
$profile = 'base' unless defined $profile;
die "profile must be base or expanded\n"
  unless $profile eq 'base' || $profile eq 'expanded';

open my $fh, '<:raw', $path or die "open $path: $!\n";
local $/;
my $nex = <$fh>;
close $fh;

sub fail {
  die "FAIL: $_[0]\n";
}

sub require_ramp {
  my ($bytes, $mask, $label) = @_;
  my $cycle = pack('C*', map { $_ ^ $mask } 0 .. 255);
  my $expected = $cycle x (length($bytes) / length($cycle));
  fail("$label bytes differ from ramp xor 0x" . sprintf('%02x', $mask))
    unless $bytes eq $expected;
}

fail('NEX is shorter than its 512-byte header') if length($nex) < 512;
fail('bad NEX signature') unless substr($nex, 0, 4) eq 'Next';

my $ram_required = ord(substr($nex, 8, 1));
my $num_banks = ord(substr($nex, 9, 1));
my @present = unpack('C112', substr($nex, 18, 112));
my @order;

for my $bank (5, 2, 0) {
  push @order, $bank if $present[$bank];
}
for my $bank (0 .. 111) {
  next if $bank == 5 || $bank == 2 || $bank == 0;
  push @order, $bank if $present[$bank];
}

fail("header count $num_banks does not match " . scalar(@order))
  unless $num_banks == @order;
my $expected_ram = $profile eq 'expanded' ? 1 : 0;
fail("RAM_Required=$ram_required, expected $expected_ram for $profile")
  unless $ram_required == $expected_ram;
fail('bank 12 (pages 24/25) is absent') unless $present[12];
fail('bank 47 (pages 94/95) is absent') unless $present[47];
if ($profile eq 'expanded') {
  fail('bank 48 (pages 96/97) is absent') unless $present[48];
} else {
  fail('NEX contains an expanded-profile bank')
    if grep { $_ >= 48 } @order;
}
fail('unexpected NEX payload length')
  unless length($nex) == 512 + 16384 * @order;

my %payload;
for my $index (0 .. $#order) {
  $payload{$order[$index]} = substr($nex, 512 + 16384 * $index, 16384);
}

require_ramp(substr($payload{12}, 0, 8192), 0x00, 'PAGE_24');
require_ramp(substr($payload{12}, 8192, 8192), 0xa7, 'PAGE_25');
require_ramp(substr($payload{47}, 8192, 8192), 0x5a, 'PAGE_95');
require_ramp(substr($payload{48}, 0, 8192), 0xc3, 'PAGE_96')
  if $profile eq 'expanded';

open my $map_fh, '<', $map_path or die "open $map_path: $!\n";
local $/;
my $map = <$map_fh>;
close $map_fh;

sub map_value {
  my ($symbol) = @_;
  my ($hex) = $map =~ /^\Q$symbol\E\s*=\s*\$([0-9a-f]+)\b/im;
  fail("map has no $symbol") unless defined $hex;
  return hex($hex);
}

fail('MMU6 probe leaf is not resident below 0xC000')
  unless map_value('_rift_probe_loaded_pages') < 0xc000;
fail('PAGE_24 map origin/size differs from 0xC000/0x2000')
  unless map_value('__PAGE_24_head') == 0xc000
      && map_value('__PAGE_24_size') == 0x2000;
fail('PAGE_25 map origin/size differs from 0xE000/0x2000')
  unless map_value('__PAGE_25_head') == 0xe000
      && map_value('__PAGE_25_size') == 0x2000;
fail('PAGE_95 map origin/size differs from 0xE000/0x2000')
  unless map_value('__PAGE_95_head') == 0xe000
      && map_value('__PAGE_95_size') == 0x2000;
if ($profile eq 'expanded') {
  fail('PAGE_96 map origin/size differs from 0xC000/0x2000')
    unless map_value('__PAGE_96_head') == 0xc000
        && map_value('__PAGE_96_size') == 0x2000;
  fail('expanded MMU6 probe leaf is not resident below 0xC000')
    unless map_value('_rift_probe_loaded_expanded_page') < 0xc000;
}

print "PASS: header advertises $num_banks payload banks in loader order\n";
print "PASS: PAGE_24 and PAGE_25 occupy the low/high halves of bank 12\n";
print "PASS: PAGE_95 occupies the high half of bank 47\n";
if ($profile eq 'expanded') {
  print "PASS: PAGE_96 occupies the low half of bank 48 and RAM_Required=1\n";
} else {
  print "PASS: RAM_Required=0 and no bank >=48 is present\n";
}
print "PASS: map pins the MMU6 leaf below 0xC000 and all page sections to 8 KiB\n";
