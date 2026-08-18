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
byte Probe_accept(Probe probe, string value);
void top_shutdown(void);
void gamma_shutdown(void);
void beta_shutdown(void);
void alpha_shutdown(void);

#endif
