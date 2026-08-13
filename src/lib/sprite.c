#include "sprite.h"
#include "pools.h"
#include <string.h>

#define SPRITE_UNBOUND 255
#define SPRITE_COUNT 128

struct rock_sprite {
  byte slot;
};

static byte visible_slots[SPRITE_COUNT / 8];
static byte transparent_index;
static byte status_flags;
static byte initialized;

void sprite_component_init(void) {
  if (initialized) return;
  memset(visible_slots, 0, sizeof(visible_slots));
  transparent_index = 0;
  status_flags = 0;
  initialized = 1;
}

void sprite_component_shutdown(void) {
  if (!initialized) return;
  memset(visible_slots, 0, sizeof(visible_slots));
  initialized = 0;
}

Sprite Sprite_new(void) {
  Sprite sprite = rock_longlived_alloc(sizeof(struct rock_sprite));
  sprite->slot = SPRITE_UNBOUND;
  return sprite;
}

byte Sprite_index(Sprite sprite, byte index) {
  if (!sprite || index >= SPRITE_COUNT) return 1;
  if (sprite->slot != SPRITE_UNBOUND && sprite->slot != index) return 2;
  sprite->slot = index;
  return 0;
}

byte Sprite_position(Sprite sprite, word x, word y) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND || x > 319 || y > 255)
    return 1;
  (void)x;
  (void)y;
  return 0;
}

byte Sprite_pattern(Sprite sprite, word asset, byte frame) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND || asset == 0) return 1;
  (void)asset;
  (void)frame;
  return 0;
}

byte Sprite_palette(Sprite sprite, byte offset) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND || offset > 15) return 1;
  (void)offset;
  return 0;
}

byte Sprite_mirror(Sprite sprite, byte x, byte y) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND || x > 1 || y > 1) return 1;
  (void)x;
  (void)y;
  return 0;
}

byte Sprite_rotate(Sprite sprite, byte quarter_turns) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND || quarter_turns > 3) return 1;
  (void)quarter_turns;
  return 0;
}

static byte valid_scale(byte value) {
  return value == 1 || value == 2 || value == 4 || value == 8;
}

byte Sprite_scale(Sprite sprite, byte x, byte y) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND ||
      !valid_scale(x) || !valid_scale(y))
    return 1;
  (void)x;
  (void)y;
  return 0;
}

byte Sprite_show(Sprite sprite) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND) return 1;
  visible_slots[sprite->slot >> 3] |= (byte)(1u << (sprite->slot & 7));
  return 0;
}

byte Sprite_hide(Sprite sprite) {
  if (!sprite || sprite->slot == SPRITE_UNBOUND) return 1;
  visible_slots[sprite->slot >> 3] &= (byte)~(1u << (sprite->slot & 7));
  return 0;
}

byte rock__tm_L6_Sprite_L7_hideall_A0(void) {
  memset(visible_slots, 0, sizeof(visible_slots));
  return 0;
}

byte rock__tm_L6_Sprite_L12_transparency_A1(byte index) {
  transparent_index = index;
  return 0;
}

byte rock__tm_L6_Sprite_L6_status_A0(void) {
  byte result = status_flags;
  status_flags = 0;
  return result;
}
