#include "error_sink.h"

#ifdef __SDCC

#include "console.h"

void rift_error_text(const char *text) {
  size_t length = 0;
  if (!text) return;
  while (text[length]) length++;
  rift_console_write(text, length);
}

void rift_error_size(size_t value) {
  char reversed[5];
  char output[5];
  size_t length = 0;
  size_t i;
  do {
    reversed[length++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0);
  for (i = 0; i < length; i++) output[i] = reversed[length - i - 1];
  rift_console_write(output, length);
}

void rift_error_int(int value) {
  unsigned int magnitude;
  if (value >= 0) {
    rift_error_size((size_t)value);
    return;
  }
  rift_console_putc('-');
  magnitude = 0u - (unsigned int)value;
  rift_error_size((size_t)magnitude);
}

void rift_error_newline(void) { rift_console_newline(); }

#else

#include <stdio.h>

void rift_error_text(const char *text) { fputs(text ? text : "", stderr); }
void rift_error_size(size_t value) { fprintf(stderr, "%zu", value); }
void rift_error_int(int value) { fprintf(stderr, "%d", value); }
void rift_error_newline(void) { fputc('\n', stderr); }

#endif
