#include "random.h"

/* 16-bit LCG state, shared by host and ZXN implementations. The
 * constants (a=25173, c=13849) are the classic Numerical Recipes
 * "quick and dirty" pair for a 16-bit modulus. Good enough for games;
 * do not use for anything that cares about statistical quality. */
static word rift_rng_state = 1;

static word rng_next(void) {
  rift_rng_state = (word)(rift_rng_state * 25173u + 13849u);
  return rift_rng_state;
}

#ifdef __SDCC

/* Seed from the Z80 R register (memory refresh counter). Combine the
 * low 7 bits of R with the current state to avoid a trivially tiny
 * seed when randomize() is called immediately after reset. */
void randomize(void) {
  unsigned char r_reg = 0;
  (void)r_reg;
  __asm
    ld  a, r
    ld  (_rift_rng_state), a
    xor a
    ld  (_rift_rng_state + 1), a
  __endasm;
  /* Force at least one step so state != 0. */
  if (rift_rng_state == 0) rift_rng_state = 1;
  (void)rng_next();
}

#else

#include <stdlib.h>

void randomize(void) {
  /* Host implementation: rand() is deterministic without srand(), which
   * is fine — randomize() on host exists for API parity, not for real
   * entropy. Tests that need determinism skip the call. */
  rift_rng_state = (word)(rand() & 0xFFFF);
  if (rift_rng_state == 0) rift_rng_state = 1;
}

#endif

byte random_byte(byte max) {
  if (max == 0) return 0;
  return (byte)(rng_next() % max);
}

word random_word(word max) {
  if (max == 0) return 0;
  return (word)(rng_next() % max);
}
