#ifndef ROCK_SPRITE_H
#define ROCK_SPRITE_H

#include "typedefs.h"

typedef struct rock_sprite *Sprite;

Sprite Sprite_new(void);
byte Sprite_index(Sprite sprite, byte index);
byte Sprite_position(Sprite sprite, word x, word y);
byte Sprite_pattern(Sprite sprite, word asset, byte frame);
byte Sprite_palette(Sprite sprite, byte offset);
byte Sprite_mirror(Sprite sprite, byte x, byte y);
byte Sprite_rotate(Sprite sprite, byte quarter_turns);
byte Sprite_scale(Sprite sprite, byte x, byte y);
byte Sprite_show(Sprite sprite);
byte Sprite_hide(Sprite sprite);

byte rock__tm_L6_Sprite_L7_hideall_A0(void);
byte rock__tm_L6_Sprite_L12_transparency_A1(byte index);
byte rock__tm_L6_Sprite_L6_status_A0(void);

void sprite_component_init(void);
void sprite_component_shutdown(void);

#endif
