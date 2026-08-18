#ifndef RIFT_SOUND_H
#define RIFT_SOUND_H

#include "typedefs.h"

/* Emit a square-wave tone on the internal speaker.
 *
 *   freq - frequency in Hz (roughly 30..10000)
 *   dur  - duration in milliseconds
 *
 * ZXN: drives the beeper via z88dk's bit_beep (ROM port $FE).
 * Host: writes the terminal bell character as a best-effort stand-in.
 */
void beep(word freq, word dur);

#endif /* RIFT_SOUND_H */
