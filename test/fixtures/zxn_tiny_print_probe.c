#include "fundefs.h"
#include "zxn_test.h"

static unsigned int screen_address(unsigned char row, unsigned char column,
                                   unsigned char scanline) {
  return (unsigned int)(0x4000 + ((unsigned int)(row & 0x18) << 8) +
      ((unsigned int)(row & 7) << 5) +
      ((unsigned int)scanline << 8) + column);
}

static int glyph_matches(unsigned char row, unsigned char column,
                         unsigned char value) {
  const unsigned char *font =
      (const unsigned char *)(0x3d00 + ((unsigned int)(value - 32) << 3));
  unsigned char scanline;
  for (scanline = 0; scanline < 8; scanline++) {
    volatile unsigned char *screen =
        (volatile unsigned char *)screen_address(row, column, scanline);
    if (*screen != font[scanline]) return 0;
  }
  return *(volatile unsigned char *)(0x5800 +
      ((unsigned int)row << 5) + column) == 7;
}

static int row_is_clear(unsigned char row) {
  unsigned char column;
  unsigned char scanline;
  for (scanline = 0; scanline < 8; scanline++) {
    for (column = 0; column < 32; column++) {
      if (*(volatile unsigned char *)screen_address(row, column, scanline) != 0)
        return 0;
    }
  }
  return 1;
}

int main(void) {
  static const char text[] = "A\nB\tC\rDE";
  unsigned char column;
  int matches = 1;

  zxn_test_begin();
  rock_print_bytes(text, 8);
  if (!glyph_matches(0, 0, 'A')) matches = 0;
  if (!glyph_matches(1, 0, 'D')) matches = 0;
  if (!glyph_matches(1, 1, 'E')) matches = 0;
  if (!glyph_matches(1, 8, 'C')) matches = 0;

  /* Move to the bottom, print a marker, then force a scroll. The marker must
   * move up one character row and the newly exposed bottom row must clear. */
  for (column = 0; column < 22; column++) rock_print_bytes("\n", 1);
  rock_print_bytes("X\n", 2);
  if (!glyph_matches(22, 0, 'X')) matches = 0;
  if (!row_is_clear(23)) matches = 0;
  if (matches)
    zxn_test_pass();
  else
    zxn_test_fail();
  zxn_test_finish();
  return 0;
}
