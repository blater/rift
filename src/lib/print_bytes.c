#include "fundefs.h"

#if defined(__SDCC) && defined(RIFT_ZXN_TINY_PRINT_DIRECT)

/* The ordinary z88dk terminal startup costs several KiB. Tiny-core programs
 * render directly into the ZX ULA display using the ROM font instead. Plain
 * literals compile the smallest writer; escaped literals opt into the small
 * control-character and scrolling layer below. */
static unsigned char rift_print_column;
static unsigned char rift_print_row;

#if defined(RIFT_ZXN_TINY_PRINT_CONTROLS)
static unsigned int rift_print_screen_address(unsigned char row,
                                              unsigned char column,
                                              unsigned char scanline) {
  return (unsigned int)(0x4000 + ((unsigned int)(row & 0x18) << 8) +
      ((unsigned int)(row & 7) << 5) +
      ((unsigned int)scanline << 8) + column);
}
#endif

static void rift_print_glyph(unsigned char value) {
  const unsigned char *font;
  volatile unsigned char *attribute;
  unsigned int screen;
  unsigned char scanline;

  if (value < 32 || value > 127) value = '?';
  font = (const unsigned char *)(0x3d00 +
      ((unsigned int)(value - 32) << 3));
#if defined(RIFT_ZXN_TINY_PRINT_CONTROLS)
  screen = rift_print_screen_address(rift_print_row, rift_print_column, 0);
#else
  screen = (unsigned int)(0x4000 +
      ((unsigned int)(rift_print_row & 0x18) << 8) +
      ((unsigned int)(rift_print_row & 7) << 5) + rift_print_column);
#endif
  for (scanline = 0; scanline < 8; scanline++) {
    *(volatile unsigned char *)(screen + ((unsigned int)scanline << 8)) =
        font[scanline];
  }
  attribute = (volatile unsigned char *)(0x5800 +
      ((unsigned int)rift_print_row << 5) + rift_print_column);
  *attribute = 7;
}

#if defined(RIFT_ZXN_TINY_PRINT_CONTROLS)

static void rift_print_clear_row(unsigned char row) {
  unsigned char column;
  unsigned char scanline;

  for (scanline = 0; scanline < 8; scanline++) {
    for (column = 0; column < 32; column++) {
      *(volatile unsigned char *)rift_print_screen_address(
          row, column, scanline) = 0;
    }
  }
  for (column = 0; column < 32; column++)
    *(volatile unsigned char *)(0x5800 + ((unsigned int)row << 5) + column) = 7;
}

static void rift_print_scroll(void) {
  unsigned char row;
  unsigned char column;
  unsigned char scanline;

  for (row = 0; row < 23; row++) {
    for (scanline = 0; scanline < 8; scanline++) {
      for (column = 0; column < 32; column++) {
        *(volatile unsigned char *)rift_print_screen_address(
            row, column, scanline) =
            *(volatile unsigned char *)rift_print_screen_address(
                (unsigned char)(row + 1), column, scanline);
      }
    }
    for (column = 0; column < 32; column++) {
      *(volatile unsigned char *)(0x5800 + ((unsigned int)row << 5) + column) =
          *(volatile unsigned char *)(0x5800 +
              ((unsigned int)(row + 1) << 5) + column);
    }
  }
  rift_print_clear_row(23);
}

static void rift_print_newline(void) {
  rift_print_column = 0;
  if (rift_print_row < 23)
    rift_print_row++;
  else
    rift_print_scroll();
}

void rift_print_bytes(const char *data, size_t length) {
  size_t i;
  for (i = 0; i < length; i++) {
    unsigned char value = (unsigned char)data[i];

    if (value == '\n') {
      rift_print_newline();
    } else if (value == '\r') {
      rift_print_column = 0;
    } else if (value == '\t') {
      rift_print_column = (unsigned char)((rift_print_column + 8) & 0xf8);
      if (rift_print_column >= 32) rift_print_newline();
    } else if (value == '\b') {
      if (rift_print_column > 0) rift_print_column--;
    } else if (value >= 32) {
      rift_print_glyph(value);
      rift_print_column++;
      if (rift_print_column == 32) rift_print_newline();
    }
  }
}

#else

void rift_print_bytes(const char *data, size_t length) {
  size_t i;
  for (i = 0; i < length; i++) {
    rift_print_glyph((unsigned char)data[i]);
    rift_print_column++;
    if (rift_print_column == 32) {
      rift_print_column = 0;
      if (rift_print_row < 23) rift_print_row++;
    }
  }
}

#endif

#if defined(__SDCC) && defined(RIFT_ZXN_LIGHT_CORE)
void rift_light_printf(const char *format, ...) {
  size_t length = 0;
  while (format[length] != 0) length++;
  rift_print_bytes(format, length);
}
#endif

#else

void rift_print_bytes(const char *data, size_t length) {
  for (size_t i = 0; i < length; i++)
    putchar((unsigned char)data[i]);
  fflush(stdout);
}

#endif
