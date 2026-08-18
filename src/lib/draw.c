#include "draw.h"
#include "plot.h"

/* --------------------------------------------------------------------
 * Shared dispatcher
 *
 * Normalises endpoints and branches on line shape. Horizontal and
 * vertical are handled by dedicated routines on the ZXN target for a
 * big speed win; on host they collapse to the single Bresenham loop
 * because terminal redraw dominates and asm tricks wouldn't show.
 * -------------------------------------------------------------------- */

#ifdef __SDCC
static void draw_hline(byte y, byte x0, byte x1);
static void draw_vline(byte x, byte y0, byte y1);
static void draw_line_general(byte x0, byte y0, byte x1, byte y1);
#else
static void draw_line_bresenham(byte x0, byte y0, byte x1, byte y1);
#endif

void draw(byte x0, byte y0, byte x1, byte y1) {
#ifdef __SDCC
  if (x0 == x1 && y0 == y1) {
    plot(x0, y0);
    return;
  }
  if (y0 == y1) {
    if (y0 >= 192) return;
    if (x0 > x1) { byte t = x0; x0 = x1; x1 = t; }
    draw_hline(y0, x0, x1);
    return;
  }
  if (x0 == x1) {
    if (y0 > y1) { byte t = y0; y0 = y1; y1 = t; }
    if (y0 >= 192) return;
    if (y1 >= 192) y1 = 191;
    draw_vline(x0, y0, y1);
    return;
  }
  draw_line_general(x0, y0, x1, y1);
#else
  draw_line_bresenham(x0, y0, x1, y1);
  plot_flush();
#endif
}

/* ==================================================================
 * ZXN target
 * ================================================================== */

#ifdef __SDCC

/* Scratch bytes read by the inline-asm blocks via absolute LD A,(nn).
 * Non-static so SDCC emits `_rift_draw_*` symbols. */
unsigned char  rift_draw_x;
unsigned char  rift_draw_y;
unsigned char  rift_draw_count;
unsigned int   rift_draw_hl;    /* 16-bit: stored HL from a PIXELAD */

/* -------- horizontal line: byte-mask walk ----------------------- */

static void pixad_into_hl(byte y, byte x) {
  rift_draw_y = y;
  rift_draw_x = x;
  __asm
    ld a, (_rift_draw_y)
    ld d, a
    ld a, (_rift_draw_x)
    ld e, a
    pixelad
    ld (_rift_draw_hl), hl
  __endasm;
}

/* Assumes x0 <= x1 and y < 192. One PIXELAD, then a contiguous byte
 * walk with leading/trailing partial masks. Safe against row-boundary
 * overflow because x is a byte (max col = 31, stays within the 32-byte
 * ULA scanline). */
static void draw_hline(byte y, byte x0, byte x1) {
  pixad_into_hl(y, x0);
  unsigned char *p = (unsigned char *)rift_draw_hl;

  byte col0  = x0 >> 3;
  byte col1  = x1 >> 3;
  byte lead  = (byte)(0xff >> (x0 & 7));
  byte trail = (byte)(0xff << (7 - (x1 & 7)));
  byte n;

  if (rift_draw_mode) {
    /* XOR: toggle the covered bits. */
    if (col0 == col1) { *p ^= (byte)(lead & trail); return; }
    *p++ ^= lead;
    n = col1 - col0 - 1;
    while (n--) { *p++ ^= 0xff; }
    *p ^= trail;
  } else {
    /* OR: additive (default). */
    if (col0 == col1) { *p |= (byte)(lead & trail); return; }
    *p++ |= lead;
    n = col1 - col0 - 1;
    while (n--) { *p++ = 0xff; }
    *p |= trail;
  }
}

/* -------- vertical line: PIXELDN loop with cached mask ---------- */

/* Assumes y0 <= y1, both in 0..191. One PIXELAD + one SETAE, then a
 * tight PIXELDN loop that reuses the cached mask in B. */
static void draw_vline(byte x, byte y0, byte y1) {
  rift_draw_x = x;
  rift_draw_y = y0;
  rift_draw_count = (byte)(y1 - y0 + 1);
  if (rift_draw_mode) {
    /* XOR loop. */
    __asm
      ld a, (_rift_draw_y)
      ld d, a
      ld a, (_rift_draw_x)
      ld e, a
      pixelad
      setae
      ld b, a
      ld a, (_rift_draw_count)
      ld c, a
    00001$:
      ld a, (hl)
      xor b
      ld (hl), a
      pixeldn
      dec c
      jr nz, 00001$
    __endasm;
  } else {
    /* OR loop (default). */
    __asm
      ld a, (_rift_draw_y)
      ld d, a
      ld a, (_rift_draw_x)
      ld e, a
      pixelad
      setae
      ld b, a
      ld a, (_rift_draw_count)
      ld c, a
    00001$:
      ld a, (hl)
      or b
      ld (hl), a
      pixeldn
      dec c
      jr nz, 00001$
    __endasm;
  }
}

/* -------- general line: Bresenham with inlined pixel write ------ */

/* Integer midpoint line. Shallow (dx >= dy) steps x every iteration and
 * steps y when the error accumulator crosses zero; steep swaps the
 * roles. The per-pixel write is a literal __asm block chosen per-pixel
 * from the current draw mode — no function call, no stack ceremony.
 * OR path costs 6 Z80 ops, XOR costs 6 as well (same shape); the C-level
 * mode branch around each block is ~4 extra ops when not predictable.
 *
 * We don't pre-dispatch on mode at the top of the function because that
 * would duplicate the entire Bresenham loop body twice. The per-pixel
 * if/else is ~50% slower but one-tenth the code. */
static void draw_line_general(byte x0, byte y0, byte x1, byte y1) {
  int x  = x0;
  int y  = y0;
  int dx = (int)x1 - (int)x0;
  int dy = (int)y1 - (int)y0;
  int sx = (dx >= 0) ? 1 : -1;
  int sy = (dy >= 0) ? 1 : -1;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;

  if (dx >= dy) {
    int err = dx >> 1;
    int i = dx;
    do {
      if ((unsigned)y < 192) {
        rift_draw_x = (byte)x;
        rift_draw_y = (byte)y;
        if (rift_draw_mode) {
          __asm
            ld a, (_rift_draw_y)
            ld d, a
            ld a, (_rift_draw_x)
            ld e, a
            pixelad
            setae
            xor (hl)
            ld (hl), a
          __endasm;
        } else {
          __asm
            ld a, (_rift_draw_y)
            ld d, a
            ld a, (_rift_draw_x)
            ld e, a
            pixelad
            setae
            or (hl)
            ld (hl), a
          __endasm;
        }
      }
      err -= dy;
      if (err < 0) { y += sy; err += dx; }
      x += sx;
    } while (i-- > 0);
  } else {
    int err = dy >> 1;
    int i = dy;
    do {
      if ((unsigned)y < 192) {
        rift_draw_x = (byte)x;
        rift_draw_y = (byte)y;
        if (rift_draw_mode) {
          __asm
            ld a, (_rift_draw_y)
            ld d, a
            ld a, (_rift_draw_x)
            ld e, a
            pixelad
            setae
            xor (hl)
            ld (hl), a
          __endasm;
        } else {
          __asm
            ld a, (_rift_draw_y)
            ld d, a
            ld a, (_rift_draw_x)
            ld e, a
            pixelad
            setae
            or (hl)
            ld (hl), a
          __endasm;
        }
      }
      err -= dx;
      if (err < 0) { x += sx; err += dy; }
      y += sy;
    } while (i-- > 0);
  }
}

#else

/* ==================================================================
 * Host target
 *
 * Single Bresenham loop. plot_nopresent handles the shadow + cell
 * redraw per pixel; plot_flush (called once from draw()) presents.
 * No dispatcher — terminal redraw is what dominates, not the per-pixel
 * math, so H/V fast paths wouldn't help.
 * ================================================================== */

static void draw_line_bresenham(byte x0, byte y0, byte x1, byte y1) {
  int x  = x0;
  int y  = y0;
  int dx = (int)x1 - (int)x0;
  int dy = (int)y1 - (int)y0;
  int sx = (dx >= 0) ? 1 : -1;
  int sy = (dy >= 0) ? 1 : -1;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;

  if (dx >= dy) {
    int err = dx >> 1;
    int i = dx;
    do {
      plot_nopresent((byte)x, (byte)y);
      err -= dy;
      if (err < 0) { y += sy; err += dx; }
      x += sx;
    } while (i-- > 0);
  } else {
    int err = dy >> 1;
    int i = dy;
    do {
      plot_nopresent((byte)x, (byte)y);
      err -= dx;
      if (err < 0) { x += sx; err += dy; }
      y += sy;
    } while (i-- > 0);
  }
}

#endif
