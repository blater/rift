#include "clock.h"

#ifdef __SDCC

#include <z80.h>
#include <intrinsic.h>

#define RIFT_ROM_FRAMES ((volatile byte *)23672u)

dword rift_clock_ticks(void) {
  byte interrupt_state = z80_get_int_state();
  byte low;
  byte middle;
  byte high;

  intrinsic_di();
  low = RIFT_ROM_FRAMES[0];
  middle = RIFT_ROM_FRAMES[1];
  high = RIFT_ROM_FRAMES[2];
  z80_set_int_state(interrupt_state);

  return (dword)low | ((dword)middle << 8) | ((dword)high << 16);
}

#else

#include <host_system_time.h>

static uint64_t host_clock_epoch;
static uint64_t host_clock_last;
static byte host_clock_started;

void rift_clock_init(void) {}
void rift_clock_shutdown(void) {}

dword rift_clock_ticks(void) {
  struct timespec now;
  uint64_t current;
  uint64_t elapsed;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return (dword)(host_clock_last & 0x00ffffffu);

  current = (uint64_t)now.tv_sec * 50u + (uint64_t)now.tv_nsec / 20000000u;
  if (!host_clock_started) {
    host_clock_epoch = current;
    host_clock_started = 1;
  }

  elapsed = current - host_clock_epoch;
  if (elapsed < host_clock_last)
    elapsed = host_clock_last;
  else
    host_clock_last = elapsed;

  return (dword)(elapsed & 0x00ffffffu);
}

#endif
