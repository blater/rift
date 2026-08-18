#ifndef RIFT_HELPERS_H
#define RIFT_HELPERS_H

#include "typedefs.h"

/* Small scalar helpers — pasta80 parity bundle.
 *
 * Ordinal:
 *   odd(x)       - true if x is odd
 *   even(x)      - true if x is even
 *
 * Byte/word split (mirrors PASTA/80 Hi/Lo/Swap):
 *   hi(w)        - high byte of a word
 *   lo(w)        - low byte of a word
 *   swap(w)      - word with bytes swapped
 *
 * Character case (ASCII only):
 *   upcase(c)    - 'a'..'z' -> 'A'..'Z', others unchanged
 *   locase(c)    - 'A'..'Z' -> 'a'..'z', others unchanged
 *
 * Absolute value:
 *   abs_int(i)   - absolute value of a signed int
 *   abs_word(w)  - identity (words are unsigned) — provided for symmetry
 *
 * These are tiny leaf functions with no platform dependencies, so
 * there is no #ifdef split inside helpers.c.
 */

byte odd(int x);
byte even(int x);

byte hi(word w);
byte lo(word w);
word swap(word w);

char upcase(char c);
char locase(char c);

int  abs_int(int x);
word abs_word(word w);

#endif /* RIFT_HELPERS_H */
