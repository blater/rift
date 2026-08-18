#ifndef RIFT_NEXTREG_H
#define RIFT_NEXTREG_H

#include "typedefs.h"

/* ZX Spectrum Next register access.
 *
 * The Next exposes platform configuration through a bank of 8-bit
 * registers selected via I/O port 0x243B and read/written via 0x253B.
 * See https://wiki.specnext.dev/NextReg for the full register map.
 *
 * next_reg_set(reg, val) - Write val to Next register reg.
 * next_reg_get(reg)      - Read Next register reg.
 *
 * cpu_speed_set(s)       - 0=3.5 MHz, 1=7 MHz, 2=14 MHz, 3=28 MHz.
 *                          Wraps Next reg 0x07.
 * cpu_speed_get()        - Returns current CPU speed code (bits 4-5
 *                          of reg 0x07).
 *
 * mmu_set(slot, page)    - Map 8 KiB page `page` into MMU slot `slot`
 *                          (0..7). Wraps Next regs 0x50..0x57.
 *
 * Host: every call updates a 256-byte shadow register file and
 * returns the stored value. cpu_speed_set/get track the shadow of
 * reg 0x07; mmu_set tracks shadow of reg 0x50+slot. Host stubs keep
 * test coverage without requiring hardware.
 */

void next_reg_set(byte reg, byte val);
byte next_reg_get(byte reg);

void cpu_speed_set(byte speed);
byte cpu_speed_get(void);

void mmu_set(byte slot, byte page);

#endif /* RIFT_NEXTREG_H */
