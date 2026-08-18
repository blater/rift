#include "plot.h"

/* Draw mode state — read by plot.c's own asm blocks (ZXN) and by
 * draw.c's inline asm via the externed symbol. Non-static so SDCC
 * emits `_rift_draw_mode`. Default 0 (additive). Written by over()
 * in ink_paper.c; no dedicated setter in this translation unit. */
unsigned char rift_draw_mode = 0;

#ifdef __SDCC

/* ZXN target: direct ULA framebuffer write via Z80N instructions.
 *
 * PIXELAD (D=Y, E=X → HL = address of pixel byte in screen memory)
 * encodes the non-linear ULA address permutation in one 8 T-state
 * instruction, so no classic-Spectrum bit math is needed.
 *
 * SETAE (E=X → A = 0x80 >> (x & 7)) builds the pixel bitmask.
 *
 * File-scope scratch bytes let the naked inline-asm routine read
 * arguments with absolute LD A,(nn) instead of fighting the SDCC
 * calling convention. Same pattern as print_at.c / ink_paper.c. */

unsigned char rift_plot_x;
unsigned char rift_plot_y;

static void plot_raw_or(void) {
  __asm
    ld a, (_rift_plot_y)
    ld d, a
    ld a, (_rift_plot_x)
    ld e, a
    pixelad
    setae
    or  (hl)
    ld  (hl), a
  __endasm;
}

static void plot_raw_xor(void) {
  __asm
    ld a, (_rift_plot_y)
    ld d, a
    ld a, (_rift_plot_x)
    ld e, a
    pixelad
    setae
    xor (hl)
    ld  (hl), a
  __endasm;
}

void plot(byte x, byte y) {
  if (y >= 192) return;
  rift_plot_x = x;
  rift_plot_y = y;
  if (rift_draw_mode) plot_raw_xor();
  else                plot_raw_or();
}

/* ZXN has no "present" step — the framebuffer IS the display. */
void plot_nopresent(byte x, byte y) { plot(x, y); }
void plot_flush(void) { }

static unsigned char rift_point_result;

static void point_raw(void) {
  __asm
    ld a, (_rift_plot_y)
    ld d, a
    ld a, (_rift_plot_x)
    ld e, a
    pixelad
    setae
    and (hl)
    ld  a, #0x00
    jr  z, _point_done
    ld  a, #0x01
  _point_done:
    ld  (_rift_point_result), a
  __endasm;
}

byte point(byte x, byte y) {
  if (y >= 192) return 0;
  rift_plot_x = x;
  rift_plot_y = y;
  point_raw();
  return rift_point_result;
}

#else

#include <stdio.h>
#include <stdint.h>
#include "host_caps.h"
#include "termbox2.h"

/* Host target: keep a 256x192 bit shadow matching the real ULA
 * framebuffer layout (1 bit per pixel, MSB = leftmost pixel within
 * its byte). Each termbox2 cell covers a 2x2 pixel block and is
 * rendered using Unicode quadrant block glyphs.
 *
 * Visible resolution on the host is therefore 128x96 — coarse, but
 * enough to eyeball shapes while developing. */

static unsigned char plot_shadow[192][32];

/* Quadrant glyph lookup. Index bits: 0=TL, 1=TR, 2=BL, 3=BR. */
static const uint32_t quadrant_glyph[16] = {
  0x0020, /*  0 ' '  */
  0x2598, /*  1 TL       ▘ */
  0x259D, /*  2    TR    ▝ */
  0x2580, /*  3 TL+TR    ▀ */
  0x2596, /*  4       BL ▖ */
  0x258C, /*  5 TL+BL    ▌ */
  0x259E, /*  6 TR+BL    ▞ */
  0x259B, /*  7 TL+TR+BL ▛ */
  0x2597, /*  8       BR ▗ */
  0x259A, /*  9 TL+BR    ▚ */
  0x2590, /* 10 TR+BR    ▐ */
  0x259C, /* 11 TL+TR+BR ▜ */
  0x2584, /* 12 BL+BR    ▄ */
  0x2599, /* 13 TL+BL+BR ▙ */
  0x259F, /* 14 TR+BL+BR ▟ */
  0x2588, /* 15 all      █ */
};

/* Read a pixel from the shadow. y is assumed in range. */
static int shadow_get(int px, int py) {
  if (px < 0 || px >= 256 || py < 0 || py >= 192) return 0;
  return (plot_shadow[py][px >> 3] >> (7 - (px & 7))) & 1;
}

static void shadow_set(int px, int py) {
  plot_shadow[py][px >> 3] |= (unsigned char)(0x80 >> (px & 7));
}

static void shadow_xor(int px, int py) {
  plot_shadow[py][px >> 3] ^= (unsigned char)(0x80 >> (px & 7));
}

/* Redraw the 2x2 cell that contains (x, y). */
static void redraw_cell(byte x, byte y) {
  int cell_x = x & ~1;
  int cell_y = y & ~1;
  int bits =
      (shadow_get(cell_x,     cell_y    ) ? 1 : 0) |
      (shadow_get(cell_x + 1, cell_y    ) ? 2 : 0) |
      (shadow_get(cell_x,     cell_y + 1) ? 4 : 0) |
      (shadow_get(cell_x + 1, cell_y + 1) ? 8 : 0);
  tb_set_cell(cell_x >> 1, cell_y >> 1,
              quadrant_glyph[bits], TB_WHITE, TB_BLACK);
}

void plot_nopresent(byte x, byte y) {
  if (y >= 192) return;

  if (rift_draw_mode) shadow_xor(x, y);
  else                shadow_set(x, y);

  if (host_caps.plot) {
    redraw_cell(x, y);
    return;
  }

  /* Fallback: capability disabled (no tty / init failed). */
  printf("plot(%u,%u)\n", (unsigned)x, (unsigned)y);
  fflush(stdout);
}

void plot_flush(void) {
  if (host_caps.plot) tb_present();
}

void plot(byte x, byte y) {
  plot_nopresent(x, y);
  plot_flush();
}

byte point(byte x, byte y) {
  if (y >= 192) return 0;
  return (byte)shadow_get(x, y);
}

#endif
