#include "ink_paper.h"
#include "plot.h"

/* Attribute kinds — used by the internal set_attr helper to pick a
 * branch without duplicating the six setter bodies. Values match the
 * ZX ROM control codes so the ZXN branch can emit them directly. */
#define ATTR_INK     16
#define ATTR_PAPER   17
#define ATTR_FLASH   18
#define ATTR_BRIGHT  19
#define ATTR_INVERSE 20
#define ATTR_OVER    21

#ifdef __SDCC

/* ZXN: the ROM maintains its own attribute sysvars on channel #2.
 * set_attr emits a two-byte control sequence through RST 10h and the
 * ROM applies the new attribute to subsequent output. We also keep a
 * local shadow so attr_fg/attr_bg have something to return for API
 * uniformity, even though print_at's ZXN branch has no consumer. */

static byte sh_ink     = 7;
static byte sh_paper   = 0;
static byte sh_bright  = 0;
static byte sh_flash   = 0;

unsigned char rift_ip_rom_byte;

static void ip_rst10_emit(void) {
  __asm
    ld a, (_rift_ip_rom_byte)
    rst 0x10
  __endasm;
}

static void set_attr(byte kind, byte val) {
  switch (kind) {
    case ATTR_INK:    sh_ink    = val; break;
    case ATTR_PAPER:  sh_paper  = val; break;
    case ATTR_BRIGHT: sh_bright = val; break;
    case ATTR_FLASH:  sh_flash  = val; break;
    default: break;
  }
  rift_ip_rom_byte = kind; ip_rst10_emit();
  rift_ip_rom_byte = val;  ip_rst10_emit();
}

unsigned int attr_fg(void) { return sh_ink  | (sh_bright ? 0x08 : 0); }
unsigned int attr_bg(void) { return sh_paper | (sh_flash  ? 0x08 : 0); }

#else

#include "termbox2.h"

/* Host: shadow the current attribute state as termbox2-ready values.
 * print_at's host branch reads attr_fg/attr_bg and passes them to
 * tb_print. Setters no-op on attribute state when the capability is
 * disabled (piped stdout / tb_init failed) — we still update the
 * shadow so read-back is consistent, but print_at won't consume it. */

/* Spectrum palette → termbox2 ANSI palette. The two palettes order
 * their colours differently (ZX is GRB-ish, termbox follows ANSI),
 * so a lookup is required. */
static const unsigned int zx_to_tb[8] = {
  TB_BLACK,    /* 0 BLACK   */
  TB_BLUE,     /* 1 BLUE    */
  TB_RED,      /* 2 RED     */
  TB_MAGENTA,  /* 3 MAGENTA */
  TB_GREEN,    /* 4 GREEN   */
  TB_CYAN,     /* 5 CYAN    */
  TB_YELLOW,   /* 6 YELLOW  */
  TB_WHITE,    /* 7 WHITE   */
};

static byte sh_ink     = 7;
static byte sh_paper   = 0;
static byte sh_bright  = 0;
static byte sh_flash   = 0;
static byte sh_inverse = 0;

static void set_attr(byte kind, byte val) {
  switch (kind) {
    case ATTR_INK:     sh_ink     = val & 0x07; break;
    case ATTR_PAPER:   sh_paper   = val & 0x07; break;
    case ATTR_BRIGHT:  sh_bright  = val ? 1 : 0; break;
    case ATTR_FLASH:   sh_flash   = val ? 1 : 0; break;
    case ATTR_INVERSE: sh_inverse = val ? 1 : 0; break;
    case ATTR_OVER:    /* no termbox2 equivalent */      break;
    default: break;
  }
}

unsigned int attr_fg(void) {
  byte ink_c   = sh_inverse ? sh_paper : sh_ink;
  unsigned int v = zx_to_tb[ink_c & 0x07];
  if (sh_bright) v |= TB_BOLD;
  if (sh_flash)  v |= TB_BLINK;
  return v;
}

unsigned int attr_bg(void) {
  byte paper_c = sh_inverse ? sh_ink : sh_paper;
  return zx_to_tb[paper_c & 0x07];
}

#endif

void ink(byte c)     { set_attr(ATTR_INK,     c); }
void paper(byte c)   { set_attr(ATTR_PAPER,   c); }
void bright(byte n)  { set_attr(ATTR_BRIGHT,  n); }
void flash(byte n)   { set_attr(ATTR_FLASH,   n); }
void inverse(byte n) { set_attr(ATTR_INVERSE, n); }
void over(byte n) {
  /* over() is the single source of truth for both pixel-level draw
   * merge mode and the ROM text channel's OVER state. Updates the
   * framebuffer-merge flag read by plot.c / draw.c inline asm, then
   * forwards to set_attr so the ROM path (ZXN) and the host shadow
   * stay consistent. */
  rift_draw_mode = n ? 1 : 0;
  set_attr(ATTR_OVER, n);
}
