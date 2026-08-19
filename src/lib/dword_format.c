#include "dword_format.h"
#include "console.h"

size_t rift_format_dword(char *buffer, dword value) {
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

void rift_print_dword(dword value) {
  char buffer[11];
  size_t length = rift_format_dword(buffer, value);
  rift_console_write(buffer, length);
}
