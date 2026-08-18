#ifndef RIFT_PRINT_AT_H
#define RIFT_PRINT_AT_H

#include "typedefs.h"

/* Positioned text output to the ZX upper screen.
 *
 * print_at(x, y, text)
 *   x, y  - 0-based character column (0..31) and row (0..23)
 *   text  - Rift string to draw starting at (x, y)
 *
 * ZXN implementation: calls ROM RST 10h directly, preceded by the AT
 * control sequence (22, row, col). This is the pasta80 approach.
 *
 * Host implementation: writes "@(x,y) text\n" to stdout for inspection.
 *
 * TODO: replace the ZXN path with direct ULA framebuffer rasterisation
 * so text output keeps working when the ROM is paged out (RAM banks over
 * $0000-$3FFF) and so custom fonts can be used. Ticketed as a follow-on
 * component; the current ROM path is a pragmatic placeholder.
 */
void print_at(byte x, byte y, string text);

#endif /* RIFT_PRINT_AT_H */
