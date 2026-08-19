#ifndef RIFT_NUMBER_FORMAT_H
#define RIFT_NUMBER_FORMAT_H

#include "typedefs.h"

size_t rift_format_byte(char *buffer, byte value);
size_t rift_format_word(char *buffer, word value);
size_t rift_format_int(char *buffer, int value);
size_t rift_format_boolean(char *buffer, boolean value);

void rift_print_boolean(boolean value);
void rift_print_byte(byte value);
void rift_print_word(word value);
void rift_print_int(int value);

#endif
