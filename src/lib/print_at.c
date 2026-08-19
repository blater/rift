#include "print_at.h"
#include "console.h"

#ifdef __SDCC

void print_at(byte x, byte y, string text) {
  if (x >= 32 || y >= 24 || text.data == 0) return;
  rift_console_set_cursor(x, y);
  rift_console_write(text.data, text.length);
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
