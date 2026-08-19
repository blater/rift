#include "console.h"

#ifdef __SDCC

/* The ordinary z88dk terminal startup costs several KiB. Rift renders into the
 * ZX ULA display using the ROM font instead. Plain tiny literals retain the
 * smallest writer; every general console profile owns controls and scrolling. */
static unsigned char rift_print_column;
static unsigned char rift_print_row;

#if defined(RIFT_ZXN_TINY_PRINT_DIRECT) && \
    !defined(RIFT_ZXN_TINY_PRINT_CONTROLS)
#define RIFT_ZXN_MINIMAL_CONSOLE 1
#endif

word rift_console_cell_address(byte column, byte row) {
  return (word)(0x4000u + ((word)(row & 0x18u) << 8) +
                ((word)(row & 7u) << 5) + column);
}

static byte rift_console_row_for_address(word address) {
  return (byte)(((address >> 8) & 0x18u) | ((address >> 5) & 7u));
}

static byte rift_console_address_is_cell(word address) {
  byte high = (byte)(address >> 8);
  return high == 0x40 || high == 0x48 || high == 0x50;
}

#if !defined(RIFT_ZXN_MINIMAL_CONSOLE)
static byte rift_console_attribute = 7;

static word rift_console_scanline_address(byte column, byte row,
                                          byte scanline) {
  return (word)(rift_console_cell_address(column, row) +
                ((word)scanline << 8));
}
#endif

void rift_console_putc_addr(word address, char value) {
  const unsigned char *font;
  volatile unsigned char *attribute;
  unsigned char scanline;

  if (!rift_console_address_is_cell(address)) return;
  if ((unsigned char)value < 32 || (unsigned char)value > 127) value = '?';
  font = (const unsigned char *)(0x3d00 +
      ((unsigned int)((unsigned char)value - 32) << 3));
  for (scanline = 0; scanline < 8; scanline++) {
    *(volatile unsigned char *)(address + ((word)scanline << 8)) =
        font[scanline];
  }
#if defined(RIFT_ZXN_MINIMAL_CONSOLE)
  attribute = (volatile unsigned char *)(0x5800 +
      ((word)rift_console_row_for_address(address) << 5) + (address & 31u));
  *attribute = 7;
#else
  attribute = (volatile unsigned char *)(0x5800 +
      ((word)rift_console_row_for_address(address) << 5) + (address & 31u));
  *attribute = rift_console_attribute;
#endif
}

void rift_console_putc_at(byte column, byte row, char value) {
  if (column >= 32 || row >= 24) return;
  rift_console_putc_addr(rift_console_cell_address(column, row), value);
}

#if !defined(RIFT_ZXN_MINIMAL_CONSOLE)

static void rift_print_clear_row(unsigned char row) {
  unsigned char column;
  unsigned char scanline;

  for (scanline = 0; scanline < 8; scanline++) {
    for (column = 0; column < 32; column++) {
      *(volatile unsigned char *)rift_console_scanline_address(
          column, row, scanline) = 0;
    }
  }
  for (column = 0; column < 32; column++)
    *(volatile unsigned char *)(0x5800 + ((word)row << 5) + column) =
        rift_console_attribute;
}

static void rift_print_scroll(void) {
  unsigned char row;
  unsigned char column;
  unsigned char scanline;

  for (row = 0; row < 23; row++) {
    for (scanline = 0; scanline < 8; scanline++) {
      for (column = 0; column < 32; column++) {
        *(volatile unsigned char *)rift_console_scanline_address(
            column, row, scanline) =
            *(volatile unsigned char *)rift_console_scanline_address(
                column, (unsigned char)(row + 1), scanline);
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

void rift_console_set_cursor(byte column, byte row) {
  if (column >= 32 || row >= 24) return;
  rift_print_column = column;
  rift_print_row = row;
}

void rift_console_set_attribute(byte attribute) {
  rift_console_attribute = attribute;
}

void rift_console_newline(void) { rift_print_newline(); }

void rift_console_clear(void) {
  byte row;
  for (row = 0; row < 24; row++) rift_print_clear_row(row);
  rift_print_column = 0;
  rift_print_row = 0;
}

void rift_console_putc(char value) {
  unsigned char byte_value = (unsigned char)value;
  if (byte_value == '\n') {
    rift_print_newline();
  } else if (byte_value == '\r') {
    rift_print_column = 0;
  } else if (byte_value == '\t') {
    rift_print_column = (unsigned char)((rift_print_column + 8) & 0xf8);
    if (rift_print_column >= 32) rift_print_newline();
  } else if (byte_value == '\b') {
    if (rift_print_column > 0) {
      rift_print_column--;
    } else if (rift_print_row > 0) {
      rift_print_row--;
      rift_print_column = 31;
    }
  } else if (byte_value >= 32) {
    rift_console_putc_at(rift_print_column, rift_print_row, value);
    rift_print_column++;
    if (rift_print_column == 32) rift_print_newline();
  }
}

void rift_console_write(const char *data, size_t length) {
  size_t i;
  if (!data) return;
  for (i = 0; i < length; i++) rift_console_putc(data[i]);
}

#else

void rift_console_set_attribute(byte attribute) { (void)attribute; }

void rift_console_write(const char *data, size_t length) {
  size_t i;
  for (i = 0; i < length; i++) {
    rift_console_putc_addr(rift_console_cell_address(
        rift_print_column, rift_print_row), data[i]);
    rift_print_column++;
    if (rift_print_column == 32) {
      rift_print_column = 0;
      if (rift_print_row < 23) rift_print_row++;
    }
  }
}

#endif

void rift_print_bytes(const char *data, size_t length) {
  rift_console_write(data, length);
}

#else

#include <stdio.h>
#include "host_caps.h"
#include "termbox2.h"

static byte rift_host_column;
static byte rift_host_row;
static byte rift_host_attribute = 7;

static const unsigned int rift_host_palette[8] = {
    TB_BLACK, TB_BLUE, TB_RED, TB_MAGENTA,
    TB_GREEN, TB_CYAN, TB_YELLOW, TB_WHITE};

word rift_console_cell_address(byte column, byte row) {
  return (word)(0x4000u + ((word)(row & 0x18u) << 8) +
                ((word)(row & 7u) << 5) + column);
}

void rift_console_set_cursor(byte column, byte row) {
  rift_host_column = column;
  rift_host_row = row;
}

void rift_console_set_attribute(byte attribute) {
  rift_host_attribute = attribute;
}

void rift_console_putc_addr(word address, char value) {
  byte high = (byte)(address >> 8);
  byte row;
  if (high != 0x40 && high != 0x48 && high != 0x50) return;
  row = (byte)(((address >> 8) & 0x18u) | ((address >> 5) & 7u));
  rift_console_putc_at((byte)(address & 31u), row, value);
}

void rift_console_putc_at(byte column, byte row, char value) {
  if (column >= 32 || row >= 24) return;
  if (host_caps.print_at) {
    byte ink = rift_host_attribute & 7u;
    byte paper = (rift_host_attribute >> 3) & 7u;
    unsigned int fg = rift_host_palette[ink];
    unsigned int bg = rift_host_palette[paper];
    if (rift_host_attribute & 0x40u) fg |= TB_BOLD;
    if (rift_host_attribute & 0x80u) fg |= TB_BLINK;
    tb_set_cell((int)column, (int)row, (uint32_t)(unsigned char)value, fg, bg);
    tb_present();
    return;
  }
  putchar((unsigned char)value);
}

void rift_console_putc(char value) {
  if (value == '\n') {
    if (!host_caps.print_at) putchar((unsigned char)value);
    rift_host_column = 0;
    if (rift_host_row < 23) rift_host_row++;
  } else if (value == '\r') {
    rift_host_column = 0;
  } else if (value == '\b') {
    if (rift_host_column > 0) {
      rift_host_column--;
    } else if (rift_host_row > 0) {
      rift_host_row--;
      rift_host_column = 31;
    }
  } else if ((unsigned char)value >= 32) {
    rift_console_putc_at(rift_host_column, rift_host_row, value);
    rift_host_column++;
    if (rift_host_column == 32) {
      rift_host_column = 0;
      if (rift_host_row < 23) rift_host_row++;
    }
  } else {
    if (!host_caps.print_at) putchar((unsigned char)value);
  }
}

void rift_console_newline(void) { rift_console_putc('\n'); }

void rift_console_clear(void) {
  putchar('\f');
  rift_host_column = 0;
  rift_host_row = 0;
}

void rift_console_write(const char *data, size_t length) {
  size_t i;
  if (!data) return;
  for (i = 0; i < length; i++) rift_console_putc(data[i]);
  fflush(stdout);
}

void rift_print_bytes(const char *data, size_t length) {
  rift_console_write(data, length);
}

#endif
