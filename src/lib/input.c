#include "input.h"

#ifdef __SDCC

/* Both routines use the standard ROM key-scan idiom (same as BASIC,
 * same as pasta80's zxrom.asm):
 *
 *   - z88dk's -startup=1 CRT leaves IM1 + the ROM interrupt handler
 *     running. The handler scans the keyboard matrix on each 50 Hz
 *     interrupt and, when it decodes a new ASCII key, writes it to
 *     LAST_K ($5C08) and sets bit 5 of the FLAGS sysvar (IY+1).
 *   - We test bit 5 to detect a fresh press, clear it after consuming,
 *     and read LAST_K for the code.
 *
 * DEPENDENCY: this only works while the ROM is paged in at $0000 and
 * IM1 is active. If a Rift program later switches to a custom IM2
 * handler or pages the ROM out, these routines stop seeing keys. */

unsigned char rift_input_byte;

byte inkey(void) {
  __asm
    ld hl, 0
    bit 5, (iy+1)
    jr z, _inkey_none
    res 5, (iy+1)
    ld a, (23560)      ; LAST_K = $5C08
    ld (_rift_input_byte), a
    ret
  _inkey_none:
    xor a
    ld (_rift_input_byte), a
  __endasm;
  return (byte)rift_input_byte;
}

byte keypress(void) {
  __asm
    res 5, (iy+1)       ; discard any stale "new key" flag
  _keypress_wait:
    halt                ; wait for next interrupt → keyboard scan runs
    bit 5, (iy+1)
    jr z, _keypress_wait
    res 5, (iy+1)
    ld a, (23560)
    ld (_rift_input_byte), a
  __endasm;
  return (byte)rift_input_byte;
}

#else

#include <stdio.h>
#include "host_caps.h"
#include "termbox2.h"

static byte event_to_byte(struct tb_event *ev) {
  if (ev->ch != 0 && ev->ch < 128) return (byte)ev->ch;
  /* Common special keys → ASCII-ish. */
  switch (ev->key) {
    case TB_KEY_ENTER:      return 13;
    case TB_KEY_BACKSPACE:
    case TB_KEY_BACKSPACE2: return 8;
    case TB_KEY_ESC:        return 27;
    case TB_KEY_SPACE:      return 32;
    case TB_KEY_TAB:        return 9;
    default: return 0;
  }
}

byte inkey(void) {
  if (host_caps.print_at) {
    struct tb_event ev;
    int r = tb_peek_event(&ev, 0);
    if (r == TB_OK && ev.type == TB_EVENT_KEY) return event_to_byte(&ev);
    return 0;
  }
  return 0;
}

byte keypress(void) {
  if (host_caps.print_at) {
    for (;;) {
      struct tb_event ev;
      if (tb_poll_event(&ev) == TB_OK && ev.type == TB_EVENT_KEY)
        return event_to_byte(&ev);
    }
  }
  {
    int c = getchar();
    if (c == EOF) return 0;
    return (byte)c;
  }
}

#endif
