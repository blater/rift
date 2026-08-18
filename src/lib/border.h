#ifndef RIFT_BORDER_H
#define RIFT_BORDER_H

#include "typedefs.h"

/* ZX Spectrum border colour control.
 *
 * border(colour)   - set the border colour (low 3 bits: 0..7).
 * border_get()     - return the last colour written via border().
 *
 * The ZXN implementation writes to port $FE. The host implementation
 * keeps a shadow byte so Rift programs can still be tested under gcc.
 */

#define BORDER_BLACK   0
#define BORDER_BLUE    1
#define BORDER_RED     2
#define BORDER_MAGENTA 3
#define BORDER_GREEN   4
#define BORDER_CYAN    5
#define BORDER_YELLOW  6
#define BORDER_WHITE   7

void border(byte colour);
byte border_get(void);

#endif /* RIFT_BORDER_H */
