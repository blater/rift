#include "sprite.h"

#include <intrinsic.h>
#include <z80.h>

#define ZESARUX_ZXI_REGISTER_PORT 0xcf3b
#define ZESARUX_ZXI_DATA_PORT 0xdf3b
#define ZESARUX_ZXI_ASCII 1
#define ZESARUX_ZXI_CONTROL 3

static byte nextreg_read(byte reg) {
  z80_outp(0x243b, reg);
  return z80_inp(0x253b);
}

static void nextreg_write(byte reg, byte value) {
  z80_outp(0x243b, reg);
  z80_outp(0x253b, value);
}

static byte iff_enabled(void) __naked {
  __asm
    ld a, i
    ld hl, #0
    ret po
    inc l
    ret
  __endasm;
}

static byte exercise_uploader(byte enable_interrupts) {
  byte original_mmu1 = nextreg_read(0x51);
  byte restored_mmu1;
  byte restored_iff;

  nextreg_write(0x51, 23);
  if (enable_interrupts) {
    intrinsic_ei();
    intrinsic_nop();
  } else {
    intrinsic_di();
  }

  /* This is the production uploader and a full PAGE_24/PAGE_25 transfer. */
  sprite_pattern_upload_banked(24, 0, 16384, 0);
  restored_mmu1 = nextreg_read(0x51) == 23;
  restored_iff = iff_enabled() == enable_interrupts;

  intrinsic_di();
  nextreg_write(0x51, original_mmu1);
  if (enable_interrupts) {
    intrinsic_ei();
    intrinsic_nop();
  }
  return restored_mmu1 && restored_iff;
}

static void emit_char(char value) {
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_ASCII);
  z80_outp(ZESARUX_ZXI_DATA_PORT, (byte)value);
}

static void emit_text(const char *text) {
  while (*text) emit_char(*text++);
}

int main(void) {
  byte disabled = exercise_uploader(0);
  byte enabled = exercise_uploader(1);

  emit_text("RIFTTEST:BEGIN\n");
  if (disabled)
    emit_text("RIFTTEST:PASS:uploader restores MMU1 and disabled IFF\n");
  else
    emit_text("RIFTTEST:FAIL:uploader failed disabled-entry restoration\n");
  if (enabled)
    emit_text("RIFTTEST:PASS:uploader restores MMU1 and enabled IFF\n");
  else
    emit_text("RIFTTEST:FAIL:uploader failed enabled-entry restoration\n");
  emit_text(disabled && enabled ? "RIFTTEST:FINISH:2:0\n"
                                : "RIFTTEST:FINISH:0:1\n");
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_CONTROL);
  z80_outp(ZESARUX_ZXI_DATA_PORT, 1);
  return disabled && enabled ? 0 : 1;
}
