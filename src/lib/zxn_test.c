#include "zxn_test.h"

#if defined(__SDCC) && defined(ROCK_ZXN_TEST)
#include <z80.h>

#define ZESARUX_ZXI_REGISTER_PORT 0xcf3b
#define ZESARUX_ZXI_DATA_PORT 0xdf3b
#define ZESARUX_ZXI_ASCII 1
#define ZESARUX_ZXI_CONTROL 3

static int pass_count;
static int fail_count;

static void emit_char(char value) {
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_ASCII);
  z80_outp(ZESARUX_ZXI_DATA_PORT, (unsigned char)value);
}

static void emit_text(const char *text) {
  while (*text) emit_char(*text++);
}

static void emit_count(int count) {
  if (count >= 10) emit_char((char)('0' + ((count / 10) % 10)));
  emit_char((char)('0' + (count % 10)));
}

void zxn_test_begin(void) {
  pass_count = 0;
  fail_count = 0;
  emit_text("ROCKTEST:BEGIN\n");
}

void zxn_test_stage(const char *stage) {
  emit_text("ROCKTEST:STAGE:");
  emit_text(stage);
  emit_char('\n');
}

void zxn_test_pass(void) {
  pass_count++;
  emit_text("ROCKTEST:PASS\n");
}

void zxn_test_fail(void) {
  fail_count++;
  emit_text("ROCKTEST:FAIL\n");
}

void zxn_test_finish(void) {
  emit_text("ROCKTEST:FINISH:");
  emit_count(pass_count);
  emit_char(':');
  emit_count(fail_count);
  emit_char('\n');
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_CONTROL);
  z80_outp(ZESARUX_ZXI_DATA_PORT, 1);
}

#else

void zxn_test_begin(void) {}
void zxn_test_stage(const char *stage) { (void)stage; }
void zxn_test_pass(void) {}
void zxn_test_fail(void) {}
void zxn_test_finish(void) {}

#endif
