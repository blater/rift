#include "input.h"

#ifdef __SDCC

/* Decoded input is built directly on the 8x5 matrix. Startup 31 places code
 * over the ROM's LAST_K/FLAGS area, so those system variables are never read.
 * One event is produced per complete press/release cycle; ambiguous chords
 * and dual-shift combinations are ignored until every ordinary key is up. */

#include "keyboard.h"

#define NO_KEY 255
#define BLOCKED_KEY 254

static byte active_key = NO_KEY;

#ifdef RIFT_ZXN_TEST
#define TEST_QUEUE_CAPACITY 272
static byte test_queue[TEST_QUEUE_CAPACITY];
static word test_queue_read;
static word test_queue_write;
#endif

static const char unshifted[KEY_COUNT] = {
    '5', '4', '3', '2', '1', '6', '7', '8', '9', '0',
    't', 'r', 'e', 'w', 'q', 'y', 'u', 'i', 'o', 'p',
    'g', 'f', 'd', 's', 'a', 'h', 'j', 'k', 'l', 13,
    'v', 'c', 'x', 'z', 0,   'b', 'n', 'm', 0,   ' '};

static const char symbol_shifted[KEY_COUNT] = {
    '%', '$', '#', '@', '!', '&', '\'', '(', ')', '_',
    '>', '<', 0,   0,   0,   '[', ']', 0,   ';', '"',
    '}', '{', '\\', '|', '~', '^', '-', '+', '=', 13,
    '/', '?', 0,   ':', 0,   '*', ',', '.', 0,   ' '};

static byte single_pressed_key(void) {
  byte key;
  byte found = NO_KEY;
  for (key = 0; key < KEY_COUNT; key++) {
    if (key == KEY_SHF || key == KEY_SYM || !key_pressed(key)) continue;
    if (found != NO_KEY) return BLOCKED_KEY;
    found = key;
  }
  return found;
}

static byte decode_key(byte key, byte caps, byte symbol) {
  char value;
  if (caps && symbol) return 0;
  if (caps && key == KEY_0) return 8;
  value = symbol ? symbol_shifted[key] : unshifted[key];
  if (caps) {
    if (value >= 'a' && value <= 'z')
      value = (char)(value - 32);
    else
      return 0;
  }
  return (byte)value;
}

static byte event_for_state(byte key, byte caps, byte symbol) {
  if (key == NO_KEY) {
    active_key = NO_KEY;
    return 0;
  }
  if (active_key != NO_KEY) return 0;
  active_key = key;
  return key == BLOCKED_KEY ? 0 : decode_key(key, caps, symbol);
}

byte inkey(void) {
  byte key;
  scan_keyboard();
  key = single_pressed_key();
  return event_for_state(key, key_pressed(KEY_SHF) != 0,
                         key_pressed(KEY_SYM) != 0);
}

byte keypress(void) {
  byte value;
#ifdef RIFT_ZXN_TEST
  if (test_queue_read < test_queue_write)
    return test_queue[test_queue_read++];
#endif
  do {
    scan_keyboard();
  } while (single_pressed_key() != NO_KEY);
  active_key = NO_KEY;
  do {
    value = inkey();
  } while (value == 0);
  return value;
}

#ifdef RIFT_ZXN_TEST
byte rift_input_test_event(byte key, byte caps, byte symbol) {
  return event_for_state(key, caps != 0, symbol != 0);
}

void rift_input_test_reset(void) {
  active_key = NO_KEY;
  test_queue_read = 0;
  test_queue_write = 0;
}

void rift_input_test_push(byte value) {
  if (test_queue_write < TEST_QUEUE_CAPACITY)
    test_queue[test_queue_write++] = value;
}
#endif

#else

#include <stdio.h>
#include "host_caps.h"
#include "termbox2.h"

static byte event_to_byte(struct tb_event *ev) {
  if (ev->ch != 0 && ev->ch < 128) return (byte)ev->ch;
  /* Common special keys → ASCII-ish. */
  switch (ev->key) {
    case TB_KEY_ENTER:      return 13;
    case TB_KEY_BACKSPACE:
    case TB_KEY_BACKSPACE2: return 8;
    case TB_KEY_ESC:        return 27;
    case TB_KEY_SPACE:      return 32;
    case TB_KEY_TAB:        return 9;
    default: return 0;
  }
}

byte inkey(void) {
  if (host_caps.print_at) {
    struct tb_event ev;
    int r = tb_peek_event(&ev, 0);
    if (r == TB_OK && ev.type == TB_EVENT_KEY) return event_to_byte(&ev);
    return 0;
  }
  return 0;
}

byte keypress(void) {
  if (host_caps.print_at) {
    for (;;) {
      struct tb_event ev;
      if (tb_poll_event(&ev) == TB_OK && ev.type == TB_EVENT_KEY)
        return event_to_byte(&ev);
    }
  }
  {
    int c = getchar();
    if (c == EOF) return 0;
    return (byte)c;
  }
}

#endif
