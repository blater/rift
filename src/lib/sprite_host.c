#include "sprite.h"

#include "sprite_host_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPRITE_COUNT 128
#define SPRITE_PATTERN_SLOTS 64
#define SPRITE_PATTERN_BYTES 256
#define SPRITE_4BPP 4
#define SPRITE_8BPP 8

typedef struct sprite_host_slot {
  word x;
  word y;
  byte pattern_base;
  byte frame;
  byte bits_per_pixel;
  byte visible;
} sprite_host_slot;

/* Rich simulation and validation state is deliberately host-only. */
static sprite_host_slot slots[SPRITE_COUNT];
static byte pattern_ram[SPRITE_PATTERN_SLOTS * SPRITE_PATTERN_BYTES];
static byte pattern_kind[SPRITE_PATTERN_SLOTS];
static byte pattern_frame_count[SPRITE_PATTERN_SLOTS];

static void sprite_host_fail(const char *message) {
  fprintf(stderr, "rift: Sprite.%s\n", message);
  exit(1);
}

static sprite_host_slot *checked_slot(Sprite sprite, const char *operation) {
  if (sprite & 0x80) sprite_host_fail(operation);
  return &slots[sprite];
}

static void upload(byte bits_per_pixel, byte pattern_base, const byte *packed,
                   word byte_count, byte frame_count) {
  word expected_slots;

  if (!packed || frame_count == 0 || byte_count == 0 ||
      byte_count % SPRITE_PATTERN_BYTES != 0)
    sprite_host_fail("asset upload requires complete 256-byte pattern slots");

  expected_slots = bits_per_pixel == SPRITE_4BPP
                       ? ((word)frame_count + 1) / 2
                       : frame_count;
  if (byte_count / SPRITE_PATTERN_BYTES != expected_slots)
    sprite_host_fail("asset upload frame count does not match packed bytes");
  if (expected_slots > SPRITE_PATTERN_SLOTS ||
      pattern_base > SPRITE_PATTERN_SLOTS - expected_slots)
    sprite_host_fail("asset upload exceeds pattern RAM");

  memcpy(pattern_ram + (word)pattern_base * SPRITE_PATTERN_BYTES, packed,
         byte_count);
  pattern_kind[pattern_base] = bits_per_pixel;
  pattern_frame_count[pattern_base] = frame_count;
}

void sprite_pattern_upload4(byte pattern_base, const byte *packed,
                            word byte_count, byte frame_count) {
  upload(SPRITE_4BPP, pattern_base, packed, byte_count, frame_count);
}

void sprite_pattern_upload8(byte pattern_base, const byte *packed,
                            word byte_count, byte frame_count) {
  upload(SPRITE_8BPP, pattern_base, packed, byte_count, frame_count);
}

void rift__im_L6_Sprite_L8_position_A2(Sprite sprite, word x, word y) {
  sprite_host_slot *slot = checked_slot(sprite, "position slot must be 0..127");

  if (x > 319) sprite_host_fail("position x must be 0..319");
  if (y > 255) sprite_host_fail("position y must be 0..255");
  slot->x = x;
  slot->y = y;
}

static void frame(Sprite sprite, byte pattern_base, byte logical_frame,
                  byte bits_per_pixel) {
  sprite_host_slot *slot = checked_slot(sprite, "frame slot must be 0..127");
  word physical_pattern = pattern_base;

  if (pattern_base >= SPRITE_PATTERN_SLOTS ||
      pattern_kind[pattern_base] != bits_per_pixel ||
      logical_frame >= pattern_frame_count[pattern_base])
    sprite_host_fail("frame is outside uploaded pattern data");
  physical_pattern += bits_per_pixel == SPRITE_4BPP ? logical_frame >> 1
                                                     : logical_frame;
  if (physical_pattern >= SPRITE_PATTERN_SLOTS)
    sprite_host_fail("frame is outside pattern RAM");

  slot->pattern_base = pattern_base;
  slot->frame = logical_frame;
  slot->bits_per_pixel = bits_per_pixel;
}

void rift__im_L6_Sprite_L6_frame4_A2(Sprite sprite, byte pattern_base,
                                     byte logical_frame) {
  frame(sprite, pattern_base, logical_frame, SPRITE_4BPP);
}

void rift__im_L6_Sprite_L6_frame8_A2(Sprite sprite, byte pattern_base,
                                     byte logical_frame) {
  frame(sprite, pattern_base, logical_frame, SPRITE_8BPP);
}

void rift__im_L6_Sprite_L4_show_A0(Sprite sprite) {
  checked_slot(sprite, "show slot must be 0..127")->visible = 1;
}

void rift__im_L6_Sprite_L4_hide_A0(Sprite sprite) {
  checked_slot(sprite, "hide slot must be 0..127")->visible = 0;
}

void rift__tm_L6_Sprite_L7_hideall_A0(void) {
  byte sprite;

  for (sprite = 0; sprite < SPRITE_COUNT; sprite++) slots[sprite].visible = 0;
}

byte sprite_host_read_slot(byte sprite, sprite_host_slot_snapshot *snapshot) {
  if ((sprite & 0x80) || !snapshot) return 0;
  snapshot->x = slots[sprite].x;
  snapshot->y = slots[sprite].y;
  snapshot->pattern_base = slots[sprite].pattern_base;
  snapshot->frame = slots[sprite].frame;
  snapshot->bits_per_pixel = slots[sprite].bits_per_pixel;
  snapshot->visible = slots[sprite].visible;
  return 1;
}

byte sprite_host_pattern_pixel(byte pattern_base, byte frame_index, byte x,
                               byte y) {
  word physical_pattern;
  word byte_index;
  byte packed;
  byte bits_per_pixel;

  if (pattern_base >= SPRITE_PATTERN_SLOTS || x >= 16 || y >= 16)
    return 0;
  bits_per_pixel = pattern_kind[pattern_base];
  if (frame_index >= pattern_frame_count[pattern_base]) return 0;
  physical_pattern = pattern_base;
  physical_pattern += bits_per_pixel == SPRITE_4BPP ? frame_index >> 1
                                                     : frame_index;
  if (physical_pattern >= SPRITE_PATTERN_SLOTS) return 0;

  byte_index = physical_pattern * SPRITE_PATTERN_BYTES;
  if (bits_per_pixel == SPRITE_8BPP)
    return pattern_ram[byte_index + (word)y * 16 + x];
  if (bits_per_pixel != SPRITE_4BPP) return 0;

  byte_index += (frame_index & 1) ? 128 : 0;
  byte_index += (word)y * 8 + (x >> 1);
  packed = pattern_ram[byte_index];
  return (x & 1) ? (packed & 0x0f) : (packed >> 4);
}

void sprite_host_reset(void) {
  memset(slots, 0, sizeof(slots));
  memset(pattern_ram, 0, sizeof(pattern_ram));
  memset(pattern_kind, 0, sizeof(pattern_kind));
  memset(pattern_frame_count, 0, sizeof(pattern_frame_count));
}
