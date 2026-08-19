#ifndef RIFT_INPUT_H
#define RIFT_INPUT_H

#include "typedefs.h"

/* Single-key input.
 *
 * inkey()    — non-blocking. Returns one decoded ASCII-like event per full
 *              press/release cycle, or 0 when no new event is available.
 * keypress() — blocking. Waits for release and then a new decodable press.
 * Non-ASCII Spectrum editing/token combinations return 0.
 */
byte inkey(void);
byte keypress(void);

#define RIFT_INPUT_MAX_LENGTH 255
string input(void);

#if defined(__SDCC) && defined(RIFT_ZXN_TEST)
/* Deterministic target-test seam. Values 255 and 254 represent no key and an
 * ambiguous ordinary-key chord. Not part of the Rift builtin surface. */
byte rift_input_test_event(byte key, byte caps, byte symbol);
void rift_input_test_reset(void);
void rift_input_test_push(byte value);
#endif

#endif /* RIFT_INPUT_H */
