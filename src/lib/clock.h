#ifndef RIFT_CLOCK_H
#define RIFT_CLOCK_H

#include "typedefs.h"

/* Return the ZX ROM FRAMES counter as an unsigned 24-bit value widened to
 * dword. The counter advances at 50 Hz and wraps modulo 2^24.
 *
 * ZXN: reads the ROM system variables at 23672..23674 atomically. Startup 1
 * enters with interrupts disabled; the clock component restores the ROM's
 * IY=$5C3A system-variable base and enables its standard IM1 handler. This
 * hook runs after startup-1 CRT initialization has finished using that region,
 * so the bytes can return to their ROM purpose before interrupts are enabled.
 * Startup 31 leaves live program code over the area and is therefore excluded.
 *
 * Host: returns an equivalent monotonic 50 Hz counter from an arbitrary
 * process-local epoch. Only equality and elapsed-tick comparisons are
 * portable between targets; the absolute epoch is deliberately unspecified.
 */
dword rift_clock_ticks(void);

/* Component lifecycle hooks emitted by the manifest. They are runtime
 * plumbing, not Rift-facing builtins. */
void rift_clock_init(void);
void rift_clock_shutdown(void);

#endif /* RIFT_CLOCK_H */
