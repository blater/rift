#include <z80.h>

#define DMA_PORT 0x006b
#define ZESARUX_ZXI_REGISTER_PORT 0xcf3b
#define ZESARUX_ZXI_DATA_PORT 0xdf3b
#define ZESARUX_ZXI_ASCII 1
#define ZESARUX_ZXI_CONTROL 3

static unsigned char source[514];
static unsigned char copy_destination[515];
static unsigned char fill_destination[8194];
static unsigned char fixed_source[3];

extern void rift_probe_interrupts_disable(void);
extern void rift_probe_interrupts_enable(void);
extern unsigned int rift_probe_interrupts_enabled(void);

static void emit_char(char value) {
  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_ASCII);
  z80_outp(ZESARUX_ZXI_DATA_PORT, (unsigned char)value);
}

static void emit_text(const char *text) {
  while (*text) emit_char(*text++);
}

static void dma_byte(unsigned char value) {
  z80_outp(DMA_PORT, value);
}

static void dma_word(unsigned int value) {
  dma_byte((unsigned char)(value & 0xff));
  dma_byte((unsigned char)(value >> 8));
}

static void dma_transfer(const void *from, void *to, unsigned int length,
                         unsigned char source_fixed) {
  dma_byte(0x83);                 /* WR6: disable */
  dma_byte(0x7d);                 /* WR0: A->B, append A and length */
  dma_word((unsigned int)from);
  dma_word(length);
  dma_byte(source_fixed ? 0x24 : 0x14); /* WR1: A fixed/increment, memory */
  dma_byte(0x10);                 /* WR2: B increment, memory */
  dma_byte(0xad);                 /* WR4: continuous, append B */
  dma_word((unsigned int)to);
  dma_byte(0x82);                 /* WR5: stop on block end */
  dma_byte(0xcf);                 /* WR6: load */
  dma_byte(0x87);                 /* WR6: enable */
}

static unsigned char probe_copy_length(unsigned int length) {
  unsigned int i;
  for (i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 37 + 11);
  for (i = 0; i < sizeof(copy_destination); ++i) copy_destination[i] = 0xee;

  dma_transfer(source + 1, copy_destination + 1, length, 0);

  if (copy_destination[0] != 0xee || copy_destination[length + 1] != 0xee)
    return 0;
  for (i = 0; i < length; ++i) {
    if (copy_destination[i + 1] != source[i + 1]) return 0;
  }
  return 1;
}

static unsigned char probe_fill_length(unsigned int length,
                                       unsigned char value) {
  unsigned int i;
  fixed_source[0] = 0x3c;
  fixed_source[1] = value;
  fixed_source[2] = 0xc3;
  for (i = 0; i < sizeof(fill_destination); ++i) fill_destination[i] = 0xee;

  dma_transfer(&fixed_source[1], fill_destination + 1, length, 1);

  if (fixed_source[0] != 0x3c || fixed_source[1] != value ||
      fixed_source[2] != 0xc3 || fill_destination[0] != 0xee ||
      fill_destination[length + 1] != 0xee)
    return 0;
  for (i = 0; i < length; ++i) {
    if (fill_destination[i + 1] != value) return 0;
  }
  return 1;
}

static unsigned char probe_interrupt_state(void) {
  unsigned char disabled_ok;
  unsigned char enabled_ok;

  rift_probe_interrupts_disable();
  disabled_ok = !rift_probe_interrupts_enabled() && probe_copy_length(512) &&
                !rift_probe_interrupts_enabled();

  rift_probe_interrupts_enable();
  enabled_ok = rift_probe_interrupts_enabled() && probe_copy_length(512) &&
               rift_probe_interrupts_enabled();
  return disabled_ok && enabled_ok;
}

int main(void) {
  static const unsigned int copy_lengths[] = {1, 2, 255, 256, 257, 512};
  static const unsigned int fill_lengths[] = {1, 255, 256, 8192};
  unsigned int i;
  unsigned char copy_ok = 1;
  unsigned char fill_ok = 1;
  unsigned char interrupt_ok;

  emit_text("RIFTTEST:BEGIN\n");
  emit_text("RIFTTEST:STAGE:dma-command-bytes\n");

  for (i = 0; i < sizeof(copy_lengths) / sizeof(copy_lengths[0]); ++i)
    copy_ok = copy_ok && probe_copy_length(copy_lengths[i]);

  for (i = 0; i < sizeof(fill_lengths) / sizeof(fill_lengths[0]); ++i) {
    fill_ok = fill_ok && probe_fill_length(fill_lengths[i], 0x5a);
    fill_ok = fill_ok && probe_fill_length(fill_lengths[i], 0xa5);
  }
  interrupt_ok = probe_interrupt_state();

  if (copy_ok)
    emit_text("RIFTTEST:PASS:zxnDMA xx6B exact lengths 1/2/255/256/257/512\n");
  else
    emit_text("RIFTTEST:FAIL:zxnDMA copy lengths:expected=exact+canaries:actual=mismatch\n");

  if (fill_ok)
    emit_text("RIFTTEST:PASS:zxnDMA fixed-source fill lengths 1/255/256/8192\n");
  else
    emit_text("RIFTTEST:FAIL:zxnDMA fixed fill:expected=exact+canaries:actual=mismatch\n");

  if (interrupt_ok)
    emit_text("RIFTTEST:PASS:zxnDMA synchronous return preserves disabled/enabled IFF\n");
  else
    emit_text("RIFTTEST:FAIL:zxnDMA IFF:expected=entry state+complete bytes:actual=mismatch\n");

  if (copy_ok && fill_ok && interrupt_ok)
    emit_text("RIFTTEST:FINISH:3:0\n");
  else
    emit_text("RIFTTEST:FINISH:0:1\n");

  z80_outp(ZESARUX_ZXI_REGISTER_PORT, ZESARUX_ZXI_CONTROL);
  z80_outp(ZESARUX_ZXI_DATA_PORT, 1);
  return 0;
}
