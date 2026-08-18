#include "host_caps.h"

rift_host_caps host_caps = {0};

#ifdef __SDCC

/* ZXN target: we are running on the real machine. Every RTL component
 * backed by hardware / ROM is always available at full fidelity. */
void rift_rtl_init(void) {
  host_caps.print_at = 1;
  host_caps.ink      = 1;
  host_caps.plot     = 1;
}

void rift_rtl_shutdown(void) {
  /* Nothing to tear down on ZXN today. */
}

void graphics_on(void) {
  /* ZXN target: graphics-capable runtime is already active. */
}

void graphics_off(void) {
  /* ZXN target: no host terminal mode to disable. */
}

#else

/* Host target: probe real capabilities at startup and install any
 * teardown hooks that the rest of the RTL relies on. */

#include <stdlib.h>
#include <unistd.h>
#include "termbox2.h"

static int host_tb_active = 0;
static int host_tb_atexit_registered = 0;

static void host_caps_disable_graphics(void) {
  host_caps.print_at = 0;
  host_caps.ink = 0;
  host_caps.plot = 0;
}

static void host_caps_enable_graphics(void) {
  host_caps.print_at = 1;
  host_caps.ink = 1;
  host_caps.plot = 1;
}

static void host_tb_teardown(void) {
  if (host_tb_active) {
    tb_present();
    tb_shutdown();
    host_tb_active = 0;
  }
  host_caps_disable_graphics();
}

void rift_rtl_init(void) {
  host_caps_disable_graphics();
}

void graphics_on(void) {
  if (host_tb_active) {
    host_caps_enable_graphics();
    return;
  }
  if (!isatty(STDOUT_FILENO)) {
    host_caps_disable_graphics();
    return;
  }
  if (tb_init() != TB_OK) {
    host_caps_disable_graphics();
    return;
  }
  host_tb_active = 1;
  if (!host_tb_atexit_registered) {
    atexit(host_tb_teardown);
    host_tb_atexit_registered = 1;
  }
  host_caps_enable_graphics();
}

void graphics_off(void) {
  host_tb_teardown();
}

void rift_rtl_shutdown(void) {
  graphics_off();
}

#endif
