#ifndef RIFT_TRIANGLE_H
#define RIFT_TRIANGLE_H

#include "typedefs.h"

/* Triangles on the ZX ULA screen.
 *
 * triangle(x1,y1, x2,y2, x3,y3)
 *   Outline only — three draw() calls between the three vertices.
 *   Uses draw()'s dispatcher, so horizontal and vertical edges take
 *   the fast paths and diagonal edges take Bresenham.
 *
 * trianglefill(x1,y1, x2,y2, x3,y3)
 *   Filled interior + outline via scanline rasterisation. Vertices
 *   are sorted by y, then each scanline emits one horizontal span
 *   via draw(xl, y, xr, y) — every row takes draw()'s byte-mask H
 *   fast path. "Current ink colour" is implicit: filled pixels are
 *   written as ink bits in the framebuffer, and the attribute cell
 *   they land in displays the colour set via ink().
 *
 * Both routines honour the sticky global draw mode from plot.h —
 * DRAW_MODE_OR (additive) or DRAW_MODE_XOR (toggle).
 */

void triangle(byte x1, byte y1, byte x2, byte y2, byte x3, byte y3);
void trianglefill(byte x1, byte y1, byte x2, byte y2, byte x3, byte y3);

#endif /* RIFT_TRIANGLE_H */
