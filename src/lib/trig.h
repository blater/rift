#ifndef RIFT_TRIG_H
#define RIFT_TRIG_H

#include "typedefs.h"

/* Fixed-point trigonometry over one unsigned-byte turn.
 *
 * phase: 0..255 maps to 0..just-under-one full turn.
 * result: signed Q8.8 value in -256..256.
 *
 * The rift_ prefix keeps these integer functions distinct from the C and
 * z88dk floating-point sin/cos symbols used by fmath.
 */
int rift_sin(byte phase);
int rift_cos(byte phase);

#endif /* RIFT_TRIG_H */
