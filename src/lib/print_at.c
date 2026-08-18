#include "print_at.h"

#ifdef __SDCC

/* Shared scratch byte for the inline-asm wrapper. Kept at file scope
 * (non-static) so SDCC emits the symbol `_rift_zx_rom_byte` which the
 * inline asm block can load with an absolute LD A,(nn). This avoids
 * depending on any particular SDCC stack/register calling convention. */
unsigned char rift_zx_rom_byte;

static void rst10_emit(void) {
  __asm
    ld a, (_rift_zx_rom_byte)
    rst 0x10
  __endasm;
}

void print_at(byte x, byte y, string text) {
  /* Emit ROM AT control sequence: 22, row, col (both 0-based).
   * RST 10h on the ZX upper-screen channel interprets this natively. */
  rift_zx_rom_byte = 22;       rst10_emit();
  rift_zx_rom_byte = y;        rst10_emit();
  rift_zx_rom_byte = x;        rst10_emit();

  if (text.data == 0) return;
  {
    size_t i;
    for (i = 0; i < text.length; i++) {
      rift_zx_rom_byte = (unsigned char)text.data[i];
      rst10_emit();
    }
  }
}

#else

#include <stdio.h>
#include <string.h>
#include "host_caps.h"
#include "ink_paper.h"
#include "termbox2.h"

/* Host path: render through termbox2 when the capability layer enabled
 * it at startup (tty stdout + successful tb_init). Otherwise fall back
 * to a plain-text line form so the test harness (piped stdout) keeps
 * working unchanged. All lifecycle (init, atexit teardown) lives in
 * host_caps.c — this file only reads the capability flag. */

void print_at(byte x, byte y, string text) {
  if (host_caps.print_at) {
    char buf[256];
    size_t n = 0;
    if (text.data != 0) {
      n = text.length < sizeof(buf) - 1 ? text.length : sizeof(buf) - 1;
      memcpy(buf, text.data, n);
    }
    buf[n] = '\0';
    tb_print((int)x, (int)y, attr_fg(), attr_bg(), buf);
    tb_present();
    return;
  }

  /* Fallback: capability disabled (no tty / init failed). */
  printf("@(%u,%u) ", (unsigned)x, (unsigned)y);
  if (text.data != 0) {
    size_t i;
    for (i = 0; i < text.length; i++) {
      putchar(text.data[i]);
    }
  }
  putchar('\n');
  fflush(stdout);
}

#endif
