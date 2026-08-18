#ifndef RIFT_HOST_CAPS_H
#define RIFT_HOST_CAPS_H

#include "typedefs.h"

/* Rift RTL capability flags.
 *
 * Every RTL component that has different fidelity between the real target
 * (ZXN) and the host development build (gcc) exposes a capability flag
 * here. Components read the flags; they never initialise themselves.
 *
 * `rift_rtl_init()` is called exactly once at program startup by the
 * generator-emitted `main()` wrapper (see generator.c `transpile_fundef`),
 * right after `fill_cmd_args`. It is the single place where host terminals
 * are opened, capability probes run, and atexit hooks installed.
 *
 * On ZXN the init is trivial: every capability is set to 1.
 * On the host target the init does real work (isatty check, tb_init, etc.).
 *
 * Components are forbidden from calling `tb_init`, `atexit`, or any other
 * lifecycle primitive directly — those responsibilities live here.
 */

typedef struct rift_host_caps {
  byte print_at;   /* positioned text output renders faithfully */
  byte ink;        /* character-cell colour attributes available */
  byte plot;       /* raster pixel plot renders via termbox2 quadrants */
  /* Future: byte keyboard; byte border; byte sound; ... */
} rift_host_caps;

extern rift_host_caps host_caps;

#if defined(__SDCC) && defined(RIFT_ZXN_TINY_CORE)
#define rift_rtl_init() ((void)0)
#define rift_rtl_shutdown() ((void)0)
#else
void rift_rtl_init(void);
void rift_rtl_shutdown(void);
#endif
void graphics_on(void);
void graphics_off(void);

#endif /* RIFT_HOST_CAPS_H */
