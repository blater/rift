#ifndef RIFT_INK_PAPER_H
#define RIFT_INK_PAPER_H

#include "typedefs.h"

/* Character-cell colour attributes for the ZX upper screen.
 *
 * Six sticky setters update the current attribute state; subsequent
 * print(x, y, text) calls render using that state. On ZXN the state is owned
 * by Rift and consumed by the direct ULA console; startup-31 code never writes
 * Spectrum ROM system variables. On the host target a shadow drives termbox2.
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

/* Read accessors used by the host branch to compose termbox2 colours. */
unsigned int attr_fg(void);
unsigned int attr_bg(void);

#endif /* RIFT_INK_PAPER_H */
