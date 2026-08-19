#include "fundefs.h"

int __to_int_byte(byte b) { return (int)b; }
byte __to_byte_int(int n) { return (byte)n; }
int __to_int_word(word w) { return (int)w; }
word __to_word_int(int n) { return (word)n; }
int __to_int_dword(dword d) { return (int)d; }
dword __to_dword_int(int n) { return (dword)n; }
int __to_int_float(float f) { return (int)f; }
byte __to_byte_word(word w) { return (byte)w; }
word __to_word_byte(byte b) { return (word)b; }
