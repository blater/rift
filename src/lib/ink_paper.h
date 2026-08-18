#ifndef RIFT_INK_PAPER_H
#define RIFT_INK_PAPER_H

#include "typedefs.h"

/* Character-cell colour attributes for the ZX upper screen.
 *
 * Six sticky setters update the current attribute state; subsequent
 * print(x, y, text) calls render using that state. On ZXN the state is
 * mirrored into the ROM via control codes on channel #2. On the host
 * target a shadow is kept here; print_at's host branch reads it back
 * through attr_fg() / attr_bg() to drive termbox2.
 */

#define COLOUR_BLACK   0
#define COLOUR_BLUE    1
#define COLOUR_RED     2
#define COLOUR_MAGENTA 3
#define COLOUR_GREEN   4
#define COLOUR_CYAN    5
#define COLOUR_YELLOW  6
#define COLOUR_WHITE   7

void ink(byte colour);
void paper(byte colour);
void bright(byte on);
void flash(byte on);
void inverse(byte on);
void over(byte on);

/* Read accessors — used by print_at's host branch to compose the
 * termbox2 fg/bg arguments. On ZXN the ROM already owns the state so
 * these return the shadow but have no consumer today. */
unsigned int attr_fg(void);
unsigned int attr_bg(void);

#endif /* RIFT_INK_PAPER_H */
