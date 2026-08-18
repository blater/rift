#include "zxn_test.h"

#if defined(__SDCC) && defined(RIFT_ZXN_TEST)
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
  emit_text("RIFTTEST:BEGIN\n");
}

void zxn_test_stage(const char *stage) {
  emit_text("RIFTTEST:STAGE:");
  emit_text(stage);
  emit_char('\n');
}

void zxn_test_pass(void) {
  pass_count++;
  emit_text("RIFTTEST:PASS\n");
}

void zxn_test_fail(void) {
  fail_count++;
  emit_text("RIFTTEST:FAIL\n");
}

void zxn_test_assert_pass(string description) {
  pass_count++;
  emit_text("RIFTTEST:PASS:");
  emit_text(description.data);
  emit_char('\n');
}

void zxn_test_assert_fail(string description, string expected, string actual) {
  fail_count++;
  emit_text("RIFTTEST:FAIL:");
  emit_text(description.data);
  emit_text(":expected=");
  emit_text(expected.data);
  emit_text(":actual=");
  emit_text(actual.data);
  emit_char('\n');
}

void zxn_test_finish(void) {
  emit_text("RIFTTEST:FINISH:");
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
void zxn_test_assert_pass(string description) { (void)description; }
void zxn_test_assert_fail(string description, string expected, string actual) {
  (void)description;
  (void)expected;
  (void)actual;
}
void zxn_test_finish(void) {}

#endif
