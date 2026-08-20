#ifndef RIFT_TEST_COMPONENT_LIFECYCLE_H
#define RIFT_TEST_COMPONENT_LIFECYCLE_H

#include "typedefs.h"

void alpha_init(void);
void beta_init(void);
void gamma_init(void);
void top_init(void);
void later_native(void);
string native_value_one(int value);
int native_value_zero(void);
typedef struct probe *Probe;
Probe Probe_new(void);
byte native_take_string(string value);
byte native_take_array(__internal_dynamic_array_t value);
Probe native_make_probe(void);
byte native_take_probe(Probe value);
byte native_order(int first, string second, int third);
byte Probe_accept(Probe probe, string value);
byte Probe_order(Probe probe, int first, string second, int third);
void top_shutdown(void);
void gamma_shutdown(void);
void beta_shutdown(void);
void alpha_shutdown(void);

#endif
