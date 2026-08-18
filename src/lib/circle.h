#ifndef RIFT_CIRCLE_H
#define RIFT_CIRCLE_H

#include "typedefs.h"

/* Raster circle outline on the ZX ULA screen.
 *
 * circle(cx, cy, r) draws the outline of a circle centred at (cx, cy)
 * with radius r using the integer midpoint circle algorithm. No fill —
 * see the (future) fill component for that.
 *
 * Pixels are plotted in 8-way symmetry: each iteration computes one
 * octant and writes eight mirrored pixels. The inner loop runs only
 * while x <= y (45° to axis), so the whole circle is ~r/sqrt(2)
 * iterations.
 *
 * Honours the sticky global draw mode from plot.h — DRAW_MODE_OR
 * (default) or DRAW_MODE_XOR (toggle; draw twice to erase).
 *
 * Pixels that fall off-screen (y >= 192) are skipped individually via
 * plot()'s built-in clip. r == 0 degenerates to a single plot; r with
 * wrap-around (cx - r underflows past 0) is handled by the unsigned
 * byte arithmetic — out-of-range pixels are clipped, not mirrored.
 */
void circle(byte cx, byte cy, byte r);

#endif /* RIFT_CIRCLE_H */
