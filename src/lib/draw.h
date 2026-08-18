#ifndef RIFT_DRAW_H
#define RIFT_DRAW_H

#include "typedefs.h"

/* Raster line draw on the ZX ULA screen.
 *
 * draw(x0, y0, x1, y1) draws a line between absolute pixel coordinates.
 * Origin is top-left, x in 0..255, y in 0..191 (pixels with y >= 192 are
 * skipped individually — no whole-line clipping in Phase 1).
 *
 * ZXN: dispatches on shape.
 *   - horizontal (y0 == y1): byte-mask walk using one PIXELAD and
 *     contiguous INC L writes with leading/trailing partial-byte masks.
 *   - vertical (x0 == x1): one PIXELAD, one SETAE, then a PIXELDN loop
 *     that reuses the cached mask — no per-row address recomputation.
 *   - general: C Bresenham (shallow or steep major axis) with an inline
 *     __asm block per pixel that performs PIXELAD+SETAE+OR+store without
 *     any function call overhead.
 *
 * Host: single C Bresenham loop that calls plot_nopresent per pixel and
 * plot_flush once at the end. Rendering goes through the same termbox2
 * quadrant-block path as plot().
 *
 * Line drawing honours the sticky global draw mode from plot.h —
 * DRAW_MODE_OR (default, additive) or DRAW_MODE_XOR (toggle).
 */

void draw(byte x0, byte y0, byte x1, byte y1);

#endif /* RIFT_DRAW_H */
