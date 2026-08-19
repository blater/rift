#ifndef RIFT_PLOT_H
#define RIFT_PLOT_H

#include "typedefs.h"

/* Raster pixel plot on the ZX ULA screen.
 *
 * plot(x, y) sets a single pixel at (x, y). Origin is top-left,
 * x in 0..255, y in 0..191 (clipped silently if y >= 192).
 *
 * ZXN: writes the ULA framebuffer at $4000-$57FF directly using the
 * Z80N PIXELAD/SETAE instructions. No ROM dependency.
 *
 * Host: maintains a 256x192 shadow bitmap and renders it through
 * termbox2 using 2x2 quadrant block glyphs when the capability is
 * enabled. Falls back to a plain-text log line otherwise so the
 * test harness keeps working.
 *
 * Plotting is additive — the existing framebuffer byte is OR'd with
 * the pixel bit. No unplot, toggle, or XOR variants in Phase 1.
 */

/* Draw mode — controls how plot/draw/circle/fill merge new pixels
 * with the existing framebuffer. Sticky global state; reads current
 * mode at each pixel write. Default additive (0).
 *
 *   0: new = old | mask  — additive (OR, default)
 *   1: new = old ^ mask  — toggle  (XOR; draw twice to erase)
 *
 * The user-facing setter is over(byte) in over.h — it matches
 * Spectrum BASIC's OVER 0/1 and drives both this pixel-level merge
 * mode and the ROM text-channel OVER state in one call.
 *
 * Internal — read by draw.c / plot.c inline asm via absolute LD A,(nn).
 */
extern unsigned char rift_draw_mode;

void plot(byte x, byte y);

/* Read a single pixel back from the screen.
 *
 * Returns 1 if the pixel at (x, y) is set, 0 otherwise. Pixels outside
 * the 256x192 display area return 0. Matches PASTA/80's Point() and
 * ZX BASIC's POINT function. */
byte point(byte x, byte y);

/* Batched-plot API used by draw.c and any future multi-pixel component.
 *
 * plot_nopresent(x, y) writes the pixel but does NOT call tb_present on
 * host, so a burst of plots amortises the (relatively expensive) screen
 * refresh. Caller MUST follow a run of plot_nopresent calls with exactly
 * one plot_flush() to make the writes visible.
 *
 * On ZXN both functions behave like plot() / a no-op: there is no
 * "present" step — the framebuffer IS the display. */
void plot_nopresent(byte x, byte y);
void plot_flush(void);

#endif /* RIFT_PLOT_H */
