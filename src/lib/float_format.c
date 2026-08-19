#include "float_format.h"
#include "console.h"
#include "dword_format.h"
#include "number_format.h"

static size_t copy_text(char *buffer, const char *text) {
  size_t length = 0;
  while (text[length]) {
    buffer[length] = text[length];
    length++;
  }
  buffer[length] = 0;
  return length;
}

static dword power10(byte exponent) {
  dword result = 1;
  while (exponent--) result *= 10u;
  return result;
}

static size_t format_scientific(char *buffer, float value, int exponent) {
  dword digits = (dword)(value * 100000.0f + 0.5f);
  char digit_buffer[11];
  char exponent_buffer[12];
  size_t digit_length;
  size_t exponent_length;
  size_t output = 0;

  if (digits >= 1000000u) {
    digits /= 10u;
    exponent++;
  }
  digit_length = rift_format_dword(digit_buffer, digits);
  while (digit_length < 6) {
    size_t i;
    for (i = digit_length; i > 0; i--) digit_buffer[i] = digit_buffer[i - 1];
    digit_buffer[0] = '0';
    digit_length++;
  }

  buffer[output++] = digit_buffer[0];
  while (digit_length > 1 && digit_buffer[digit_length - 1] == '0')
    digit_length--;
  if (digit_length > 1) {
    size_t i;
    buffer[output++] = '.';
    for (i = 1; i < digit_length; i++) buffer[output++] = digit_buffer[i];
  }
  buffer[output++] = 'e';
  if (exponent >= 0) buffer[output++] = '+';
  exponent_length = rift_format_int(exponent_buffer, exponent);
  for (size_t i = 0; i < exponent_length; i++)
    buffer[output++] = exponent_buffer[i];
  buffer[output] = 0;
  return output;
}

static size_t format_fixed(char *buffer, float value, int exponent) {
  byte decimals = exponent >= 5 ? 0 : (byte)(5 - exponent);
  dword scale = power10(decimals);
  dword scaled = (dword)(value * (float)scale + 0.5f);
  char digits[11];
  size_t length = rift_format_dword(digits, scaled);
  size_t output = 0;

  if (decimals == 0) return copy_text(buffer, digits);
  if (length <= decimals) {
    size_t zeros = (size_t)decimals - length;
    buffer[output++] = '0';
    buffer[output++] = '.';
    while (zeros--) buffer[output++] = '0';
    for (size_t i = 0; i < length; i++) buffer[output++] = digits[i];
  } else {
    size_t integer_length = length - decimals;
    for (size_t i = 0; i < integer_length; i++)
      buffer[output++] = digits[i];
    buffer[output++] = '.';
    for (size_t i = integer_length; i < length; i++)
      buffer[output++] = digits[i];
  }
  while (output > 0 && buffer[output - 1] == '0') output--;
  if (output > 0 && buffer[output - 1] == '.') output--;
  buffer[output] = 0;
  return output;
}

size_t rift_format_float(char *buffer, float value) {
  int negative = value < 0.0f;
  int exponent = 0;
  size_t length;

  if (value != value) return copy_text(buffer, "nan");
  if (negative) value = 0.0f - value;
  if (value == 0.0f) return copy_text(buffer, "0");

  while (value >= 10.0f && exponent < 38) {
    value /= 10.0f;
    exponent++;
  }
  while (value < 1.0f && exponent > -38) {
    value *= 10.0f;
    exponent--;
  }
  if ((exponent == 38 && value >= 10.0f) || value != value)
    return copy_text(buffer, negative ? "-inf" : "inf");

  if (negative) buffer[0] = '-';
  if (exponent >= 6 || exponent <= -5)
    length = format_scientific(buffer + negative, value, exponent);
  else {
    float fixed_value = value;
    if (exponent > 0)
      fixed_value *= (float)power10((byte)exponent);
    else if (exponent < 0)
      fixed_value /= (float)power10((byte)(0 - exponent));
    length = format_fixed(buffer + negative, fixed_value, exponent);
  }
  return length + (size_t)negative;
}

void rift_print_float(float value) {
  char buffer[20];
  size_t length = rift_format_float(buffer, value);
  rift_console_write(buffer, length);
}
