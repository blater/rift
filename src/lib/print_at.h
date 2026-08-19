#ifndef RIFT_PRINT_AT_H
#define RIFT_PRINT_AT_H

#include "typedefs.h"

/* Positioned text output to the ZX upper screen.
 *
 * print_at(x, y, text)
 *   x, y  - 0-based character column (0..31) and row (0..23)
 *   text  - Rift string to draw starting at (x, y)
 *
 * ZXN implementation: writes ROM-font glyphs through the shared direct ULA
 * console. It does not call ROM output routines or use their system state.
 *
 * Host implementation: writes "@(x,y) text\n" to stdout for inspection.
 *
 * The glyph source is the standard ROM font at $3D00, so a future custom-font
 * API may supply another source while retaining the direct framebuffer path.
 */
void print_at(byte x, byte y, string text);

#endif /* RIFT_PRINT_AT_H */
