#ifndef RIFT_FMATH_H
#define RIFT_FMATH_H

#include "typedefs.h"

/* Floating-point math primitives.
 * Host: wraps sinf/cosf/sqrtf.  ZXN: wraps z88dk genmath via -lm.
 * Names are f-prefixed to avoid colliding with libm symbols.
 */

float fsin(float x);
float fcos(float x);
float fsqrt(float x);
float fabs_float(float x);
float fpi(void);

#endif /* RIFT_FMATH_H */
