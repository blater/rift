#ifndef RIFT_CONSOLE_H
#define RIFT_CONSOLE_H

#include "typedefs.h"

/* Length-aware console output shared by ordinary, positioned, and line-input
 * paths. ZXN implementations write the ULA display directly; host builds keep
 * their existing stdio/termbox adapters. */
void rift_print_bytes(const char *data, size_t length);
void rift_console_write(const char *data, size_t length);
void rift_console_putc(char value);
void rift_console_newline(void);
void rift_console_set_cursor(byte column, byte row);
void rift_console_set_attribute(byte attribute);
word rift_console_cell_address(byte column, byte row);
void rift_console_putc_addr(word address, char value);
void rift_console_putc_at(byte column, byte row, char value);
void rift_console_clear(void);

#endif
