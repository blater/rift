#include "keyboard.h"

#ifdef __SDCC

/* ZXN target: real implementation lives in src/lib/zxn/keyboard.asm.
 * The asm file defines `_scan_keyboard` and a 40-byte buffer `keybuffer`.
 * We expose the buffer here via an extern declaration using its PUBLIC alias. */
extern unsigned char ZK_BUFFER[KEY_COUNT];
extern void scan_keyboard(void);

byte key_pressed(byte key_id) {
  if (key_id >= KEY_COUNT) return 0;
  return ZK_BUFFER[key_id];
}

#else

/* Host target: no hardware. Maintain a zeroed buffer so Rift programs
 * that call into the keyboard API still link and run deterministically. */
static unsigned char host_buf[KEY_COUNT];

void scan_keyboard(void) {
  /* no-op on host */
}

byte key_pressed(byte key_id) {
  if (key_id >= KEY_COUNT) return 0;
  return host_buf[key_id];
}

#endif
