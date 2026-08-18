#ifndef RIFT_RANDOM_H
#define RIFT_RANDOM_H

#include "typedefs.h"

/* Pseudo-random number generator.
 *
 * randomize()       - Reseed from a platform-specific entropy source.
 *                     ZXN: Z80 R register (refresh counter).
 *                     Host: time(NULL).
 *                     Call once at program start for non-repeating runs.
 *                     Without a prior randomize() call the sequence is
 *                     deterministic (seed = 1) — handy for tests.
 * random_byte(max)  - Return a byte in 0..max-1. If max == 0 returns 0.
 * random_word(max)  - Return a word in 0..max-1. If max == 0 returns 0.
 *
 * Internal generator is an 8/16-bit linear congruential generator with
 * constants chosen to match Numerical Recipes' quick-and-dirty LCG.
 * Not cryptographically secure; fine for games, demos, mazes, etc.
 */

void randomize(void);
byte random_byte(byte max);
word random_word(word max);

#endif /* RIFT_RANDOM_H */
