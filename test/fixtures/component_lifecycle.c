#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "component_lifecycle.h"

static char events[16];
static int event_count;

static void event(char value) {
  if (event_count >= (int)sizeof(events) - 1) exit(90);
  events[event_count++] = value;
  events[event_count] = '\0';
}

void alpha_init(void) { event('A'); }
void beta_init(void) { event('B'); }
void gamma_init(void) { event('G'); }
void top_init(void) { event('T'); }
void later_native(void) { event('U'); }
string native_value_one(int ignored) {
  (void)ignored;
  return (string){0};
}
int native_value_zero(void) { return 0; }
struct probe { int unused; };
Probe Probe_new(void) { return NULL; }
byte Probe_accept(Probe probe, string value) {
  (void)probe;
  return value.length == 13 ? 0 : 1;
}
void top_shutdown(void) { event('t'); }
void gamma_shutdown(void) { event('g'); }
void beta_shutdown(void) { event('b'); }

void alpha_shutdown(void) {
  if (strcmp(events, "ABGTUtgb") != 0) {
    fprintf(stderr, "wrong lifecycle sequence: %s\n", events);
    exit(91);
  }
  puts("PASS: component lifecycle order");
}
