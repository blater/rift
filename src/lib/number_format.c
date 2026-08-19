#include "number_format.h"
#include "console.h"

static size_t format_unsigned_word(char *buffer, word value) {
  char reversed[5];
  size_t length = 0;
  size_t i;

  do {
    reversed[length++] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0);

  for (i = 0; i < length; i++) buffer[i] = reversed[length - i - 1];
  buffer[length] = 0;
  return length;
}

#ifndef __SDCC
static size_t format_unsigned_int(char *buffer, unsigned int value) {
  char reversed[10];
  size_t length = 0;
  size_t i;
  do {
    reversed[length++] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0);
  for (i = 0; i < length; i++) buffer[i] = reversed[length - i - 1];
  buffer[length] = 0;
  return length;
}
#endif

size_t rift_format_byte(char *buffer, byte value) {
  return format_unsigned_word(buffer, (word)value);
}

size_t rift_format_word(char *buffer, word value) {
  return format_unsigned_word(buffer, value);
}

size_t rift_format_int(char *buffer, int value) {
  unsigned int magnitude;
  size_t length;
#ifdef __SDCC
  if (value >= 0) return format_unsigned_word(buffer, (word)value);
#else
  if (value >= 0) return format_unsigned_int(buffer, (unsigned int)value);
#endif
  buffer[0] = '-';
  magnitude = 0u - (unsigned int)value;
#ifdef __SDCC
  length = format_unsigned_word(buffer + 1, (word)magnitude);
#else
  length = format_unsigned_int(buffer + 1, magnitude);
#endif
  return length + 1;
}

size_t rift_format_boolean(char *buffer, boolean value) {
  const char *text = value ? "true" : "false";
  size_t length = value ? 4 : 5;
  size_t i;
  for (i = 0; i < length; i++) buffer[i] = text[i];
  buffer[length] = 0;
  return length;
}

void rift_print_boolean(boolean value) {
  char buffer[6];
  size_t length = rift_format_boolean(buffer, value);
  rift_console_write(buffer, length);
}

void rift_print_byte(byte value) {
  char buffer[4];
  size_t length = rift_format_byte(buffer, value);
  rift_console_write(buffer, length);
}

void rift_print_word(word value) {
  char buffer[6];
  size_t length = rift_format_word(buffer, value);
  rift_console_write(buffer, length);
}

void rift_print_int(int value) {
  char buffer[12];
  size_t length = rift_format_int(buffer, value);
  rift_console_write(buffer, length);
}
