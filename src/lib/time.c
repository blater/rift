#include "time.h"

#ifdef __SDCC

#include <z80.h>

void rift_sleep(word ms) {
  z80_delay_ms(ms);
}

#else

/* Host: usleep from unistd.h. No name clash now that the C symbol is
 * `rift_sleep`, not `sleep`. */
#include <unistd.h>

void rift_sleep(word ms) {
  usleep((useconds_t)ms * 1000);
}

#endif
