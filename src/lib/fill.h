#ifndef RIFT_FILL_H
#define RIFT_FILL_H

#include "typedefs.h"

/* Filled rectangle on the ZX ULA screen.
 *
 * fill(x0, y0, x1, y1) paints every pixel in the axis-aligned
 * rectangle bounded by the two endpoints (inclusive on all edges).
 * Endpoint order does not matter — the component sorts.
 *
 * Implemented as a vertical sweep of horizontal lines, so it reuses
 * draw()'s H-fast-path byte-mask walk: one PIXELAD per row, then a
 * contiguous byte store across the span. A 128×96 fill runs in
 * ~96 * 16 byte-writes on ZXN — the fastest rectangle path we can
 * get without touching PIXELDN span tricks.
 *
 * Honours the sticky global draw mode set via over(0|1) in
 * ink_paper.h — 0 is additive (default), 1 toggles every covered
 * pixel (fill twice to erase).
 *
 * Pixels with y >= 192 are skipped per-row. Out-of-order endpoints
 * (x1 < x0 or y1 < y0) are normalised before iteration.
 *
 * The 4-arg signature is reserved for the rectangular shape; if flood
 * fill is ever added it will be a 2-arg overload `fill(x, y)` picked
 * by arity, not a separate name.
 */
void fill(byte x0, byte y0, byte x1, byte y1);

#endif /* RIFT_FILL_H */
