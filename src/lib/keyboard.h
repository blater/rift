#ifndef RIFT_KEYBOARD_H
#define RIFT_KEYBOARD_H

#include "typedefs.h"

/* ZX Spectrum keyboard scanner.
 *
 * scan_keyboard()         - Latch the 8x5 key matrix into an internal buffer.
 * key_pressed(key_id)     - Return press strength for key_id (0 = not pressed).
 *
 * Key IDs below are stable integers. The order matches the physical
 * half-row scan order used by src/lib/zxn/keyboard.asm.
 */

#define KEY_5     0
#define KEY_4     1
#define KEY_3     2
#define KEY_2     3
#define KEY_1     4

#define KEY_6     5
#define KEY_7     6
#define KEY_8     7
#define KEY_9     8
#define KEY_0     9

#define KEY_T     10
#define KEY_R     11
#define KEY_E     12
#define KEY_W     13
#define KEY_Q     14

#define KEY_Y     15
#define KEY_U     16
#define KEY_I     17
#define KEY_O     18
#define KEY_P     19

#define KEY_G     20
#define KEY_F     21
#define KEY_D     22
#define KEY_S     23
#define KEY_A     24

#define KEY_H     25
#define KEY_J     26
#define KEY_K     27
#define KEY_L     28
#define KEY_ENT   29

#define KEY_V     30
#define KEY_C     31
#define KEY_X     32
#define KEY_Z     33
#define KEY_SHF   34

#define KEY_B     35
#define KEY_N     36
#define KEY_M     37
#define KEY_SYM   38
#define KEY_SPC   39

#define KEY_COUNT 40

void scan_keyboard(void);
byte key_pressed(byte key_id);

#endif /* RIFT_KEYBOARD_H */
