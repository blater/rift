#include "nextreg.h"

/* Scratch bytes for the inline-asm routines — same pattern as plot.c.
 * SDCC's calling convention is awkward to read from naked asm, so we
 * stash args in file-scope statics and load via absolute LD A,(nn). */
static byte rift_nr_reg;
static byte rift_nr_val;

#ifdef __SDCC

/* ZXN target: port 0x243B selects the register number, 0x253B reads
 * or writes its value. Z80N has the NEXTREG instruction which does
 * both ports in one shot, but the classic OUT-then-OUT sequence is
 * shorter to express here and works identically. */

void next_reg_set(byte reg, byte val) {
  rift_nr_reg = reg;
  rift_nr_val = val;
  __asm
    ld  a, (_rift_nr_reg)
    ld  bc, #0x243B
    out (c), a
    ld  a, (_rift_nr_val)
    ld  bc, #0x253B
    out (c), a
  __endasm;
}

byte next_reg_get(byte reg) {
  rift_nr_reg = reg;
  __asm
    ld  a, (_rift_nr_reg)
    ld  bc, #0x243B
    out (c), a
    ld  bc, #0x253B
    in  a, (c)
    ld  (_rift_nr_val), a
  __endasm;
  return rift_nr_val;
}

#else

/* Host: keep a 256-byte shadow of the Next register file. */
static byte rift_nr_shadow[256];

void next_reg_set(byte reg, byte val) {
  rift_nr_shadow[reg] = val;
}

byte next_reg_get(byte reg) {
  return rift_nr_shadow[reg];
}

#endif

/* Shared, portable wrappers built on next_reg_set / next_reg_get.
 * Keeping these in the shared section means the ZXN and host paths
 * get identical semantics for free. */

#define NR_TURBO_MODE  0x07
#define NR_MMU_SLOT0   0x50

void cpu_speed_set(byte speed) {
  next_reg_set(NR_TURBO_MODE, (byte)(speed & 0x03));
}

byte cpu_speed_get(void) {
  return (byte)(next_reg_get(NR_TURBO_MODE) & 0x03);
}

void mmu_set(byte slot, byte page) {
  if (slot > 7) return;
  next_reg_set((byte)(NR_MMU_SLOT0 + slot), page);
}
