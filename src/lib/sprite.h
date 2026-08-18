#ifndef RIFT_SPRITE_H
#define RIFT_SPRITE_H

#include "typedefs.h"

/* A Sprite is only its hardware slot.  The compiler lowers Sprite(byte)
 * directly to this byte representation; there is no runtime constructor or
 * handle allocation. */
typedef byte Sprite;

void rift__im_L6_Sprite_L8_position_A2(Sprite sprite, word x, word y);
void rift__im_L6_Sprite_L6_frame4_A2(Sprite sprite, byte pattern_base,
                                     byte frame);
void rift__im_L6_Sprite_L6_frame8_A2(Sprite sprite, byte pattern_base,
                                     byte frame);
void rift__im_L6_Sprite_L4_show_A0(Sprite sprite);
void rift__im_L6_Sprite_L4_hide_A0(Sprite sprite);
void rift__tm_L6_Sprite_L7_hideall_A0(void);

#ifdef __SDCC
/* Upload one packed, physical-slot-aligned span from NEX extra pages.  The
 * helper restores MMU1 and the caller's interrupt state before returning. */
void sprite_pattern_upload_banked(byte source_page, word source_offset,
                                  word byte_count, byte pattern_base);
#else
/* Host asset initialization retains logical counts for dynamic validation.
 * This metadata is host-only; the ZXN target keeps no asset descriptors. */
void sprite_pattern_upload4(byte pattern_base, const byte *packed,
                            word byte_count, byte frame_count);
void sprite_pattern_upload8(byte pattern_base, const byte *packed,
                            word byte_count, byte frame_count);
#endif

#endif
