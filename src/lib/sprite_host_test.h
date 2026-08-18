#ifndef RIFT_SPRITE_HOST_TEST_H
#define RIFT_SPRITE_HOST_TEST_H

#include "typedefs.h"

typedef struct sprite_host_slot_snapshot {
  word x;
  word y;
  byte pattern_base;
  byte frame;
  byte bits_per_pixel;
  byte visible;
} sprite_host_slot_snapshot;

byte sprite_host_read_slot(byte slot, sprite_host_slot_snapshot *snapshot);
byte sprite_host_pattern_pixel(byte pattern_base, byte frame, byte x, byte y);
void sprite_host_reset(void);

#endif
