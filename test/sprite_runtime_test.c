#include "sprite.h"
#include "sprite_host_test.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define EXPECT(condition, message)                                              \
  do {                                                                          \
    if (!(condition)) {                                                          \
      fprintf(stderr, "FAIL: %s\n", message);                                  \
      failures++;                                                               \
    }                                                                            \
  } while (0)

static void load_patterns(void) {
  byte packed4[256];
  byte packed8[512];
  byte odd4[256];

  memset(packed4, 0x12, 128);
  memset(packed4 + 128, 0xab, 128);
  memset(packed8, 0x34, 256);
  memset(packed8 + 256, 0xcd, 256);
  memset(odd4, 0x56, sizeof(odd4));

  sprite_pattern_upload4(2, packed4, sizeof(packed4), 2);
  sprite_pattern_upload8(3, packed8, sizeof(packed8), 2);
  sprite_pattern_upload4(5, odd4, sizeof(odd4), 1);
}

static void exercise_valid_operations(void) {
  sprite_host_slot_snapshot slot;
  Sprite alien = 0;
  Sprite alias = alien;
  Sprite boundary = 127;

  sprite_host_reset();
  load_patterns();

  EXPECT(sprite_host_pattern_pixel(2, 0, 0, 0) == 1,
         "4bpp frame zero uses the first packed half");
  EXPECT(sprite_host_pattern_pixel(2, 0, 1, 0) == 2,
         "4bpp pixels retain left/right nibble order");
  EXPECT(sprite_host_pattern_pixel(2, 1, 0, 0) == 10,
         "4bpp frame one uses the second packed half");
  EXPECT(sprite_host_pattern_pixel(2, 1, 1, 0) == 11,
         "4bpp second-half right nibble is preserved");
  EXPECT(sprite_host_pattern_pixel(3, 0, 0, 0) == 0x34,
         "8bpp frame zero stores one byte per pixel");
  EXPECT(sprite_host_pattern_pixel(3, 1, 15, 15) == 0xcd,
         "8bpp frame one occupies the following physical pattern");

  rift__im_L6_Sprite_L8_position_A2(alien, 20, 30);
  rift__im_L6_Sprite_L6_frame4_A2(alien, 2, 0);
  rift__im_L6_Sprite_L4_show_A0(alien);
  EXPECT(sprite_host_read_slot(alien, &slot) && slot.visible && slot.x == 20 &&
             slot.y == 30 && slot.pattern_base == 2 && slot.frame == 0 &&
             slot.bits_per_pixel == 4,
         "separated operations compose one visible 4bpp sprite");

  rift__im_L6_Sprite_L8_position_A2(alias, 40, 50);
  EXPECT(sprite_host_read_slot(alien, &slot) && slot.visible && slot.x == 40 &&
             slot.y == 50 && slot.pattern_base == 2 && slot.frame == 0,
         "a copied Sprite byte aliases the same hardware slot");

  rift__im_L6_Sprite_L6_frame4_A2(alien, 2, 1);
  EXPECT(sprite_host_read_slot(alien, &slot) && slot.visible && slot.x == 40 &&
             slot.y == 50 && slot.frame == 1,
         "frame preserves position and visibility");

  rift__im_L6_Sprite_L4_hide_A0(alien);
  EXPECT(sprite_host_read_slot(alien, &slot) && !slot.visible && slot.x == 40 &&
             slot.y == 50 && slot.pattern_base == 2 && slot.frame == 1,
         "hide changes visibility only");
  rift__im_L6_Sprite_L4_show_A0(alien);
  EXPECT(sprite_host_read_slot(alien, &slot) && slot.visible && slot.frame == 1,
         "show restores the retained frame");

  rift__im_L6_Sprite_L8_position_A2(boundary, 319, 255);
  rift__im_L6_Sprite_L6_frame8_A2(boundary, 3, 1);
  rift__im_L6_Sprite_L4_show_A0(boundary);
  EXPECT(sprite_host_read_slot(boundary, &slot) && slot.visible &&
             slot.x == 319 && slot.y == 255 && slot.pattern_base == 3 &&
             slot.frame == 1 && slot.bits_per_pixel == 8,
         "8bpp and every public coordinate/slot upper boundary work");

  rift__tm_L6_Sprite_L7_hideall_A0();
  EXPECT(sprite_host_read_slot(alien, &slot) && !slot.visible && slot.x == 40 &&
             slot.y == 50 && slot.pattern_base == 2 && slot.frame == 1,
         "hideall retains 4bpp position and selected frame");
  EXPECT(sprite_host_read_slot(boundary, &slot) && !slot.visible &&
             slot.x == 319 && slot.y == 255 && slot.pattern_base == 3 &&
             slot.frame == 1 && slot.bits_per_pixel == 8,
         "hideall retains 8bpp position and selected frame");
}

int main(int argc, char **argv) {
  if (argc == 1) {
    exercise_valid_operations();
    if (failures) return 1;
    puts("PASS: byte-backed Sprite operations preserve state for 4/8bpp");
    return 0;
  }

  sprite_host_reset();
  load_patterns();
  if (strcmp(argv[1], "position-slot") == 0)
    rift__im_L6_Sprite_L8_position_A2(128, 0, 0);
  else if (strcmp(argv[1], "frame-slot") == 0)
    rift__im_L6_Sprite_L6_frame4_A2(128, 2, 0);
  else if (strcmp(argv[1], "show-slot") == 0)
    rift__im_L6_Sprite_L4_show_A0(128);
  else if (strcmp(argv[1], "hide-slot") == 0)
    rift__im_L6_Sprite_L4_hide_A0(128);
  else if (strcmp(argv[1], "x") == 0)
    rift__im_L6_Sprite_L8_position_A2(0, 320, 0);
  else if (strcmp(argv[1], "y") == 0)
    rift__im_L6_Sprite_L8_position_A2(0, 0, 256);
  else if (strcmp(argv[1], "frame4") == 0)
    rift__im_L6_Sprite_L6_frame4_A2(0, 5, 1);
  else if (strcmp(argv[1], "frame8") == 0)
    rift__im_L6_Sprite_L6_frame8_A2(0, 3, 2);
  else
    return 2;
  return 2;
}
