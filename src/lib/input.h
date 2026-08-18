#ifndef RIFT_INPUT_H
#define RIFT_INPUT_H

#include "typedefs.h"

/* Single-key input.
 *
 * inkey()    — non-blocking. Returns the ASCII code of the currently
 *              pressed key, or 0 if no key is held.
 * keypress() — blocking. Waits until a key is pressed and returns its
 *              ASCII code.
 */
byte inkey(void);
byte keypress(void);

#endif /* RIFT_INPUT_H */
