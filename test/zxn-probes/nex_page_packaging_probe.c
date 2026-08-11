#include <z80.h>

#define ZESARUX_ZXI_REGISTER_PORT 0xcf3b
#define ZESARUX_ZXI_DATA_PORT 0xdf3b
#define ZESARUX_ZXI_ASCII 1
#define ZESARUX_ZXI_CONTROL 3

extern unsigned int rock_probe_loaded_pages(void);
#ifdef ROCK_PROBE_EXPANDED
extern unsigned int rock_probe_loaded_expanded_page(void);
#endif

static void emit_char(char value) {
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_ASCII);
  z80_outp(ZESARUX_ZXI_DATA_PORT, (unsigned char)value);
}

static void emit_text(const char *text) {
  while (*text) emit_char(*text++);
}

int main(void) {
  unsigned int passed = rock_probe_loaded_pages();
#ifdef ROCK_PROBE_EXPANDED
  passed = passed && rock_probe_loaded_expanded_page();
#endif

  emit_text("ROCKTEST:BEGIN\n");
  emit_text("ROCKTEST:STAGE:nex-page-loader\n");

  if (passed) {
#ifdef ROCK_PROBE_EXPANDED
    emit_text("ROCKTEST:PASS:NEX PAGE_24/PAGE_25/PAGE_95/PAGE_96 loaded through MMU6\n");
#else
    emit_text("ROCKTEST:PASS:NEX PAGE_24/PAGE_25/PAGE_95 loaded through MMU6\n");
#endif
    emit_text("ROCKTEST:FINISH:1:0\n");
  } else {
    emit_text("ROCKTEST:FAIL:NEX page bytes:expected=3c/a7/5a:actual=mismatch\n");
    emit_text("ROCKTEST:FINISH:0:1\n");
  }

  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_CONTROL);
  z80_outp(ZESARUX_ZXI_DATA_PORT, 1);
  return 0;
}
