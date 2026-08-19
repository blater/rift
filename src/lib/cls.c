#include "cls.h"

#ifdef __SDCC

#include "console.h"

/* Clear all ULA bitmap and attribute bytes using the current permanent
 * ROM attribute. This avoids ROM entry points whose channel/startup
 * preconditions do not hold for every Rift program. */
void cls(void) {
  rift_console_clear();
}

#else

#include <stdio.h>
#include "host_caps.h"
#include "termbox2.h"

void cls(void) {
  if (host_caps.print_at) {
    tb_clear();
    tb_present();
    return;
  }
  /* Fallback: form-feed so piped stdout stays deterministic-ish. */
  putchar('\f');
  fflush(stdout);
}

#endif
