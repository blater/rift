#include "trig.h"

/* floor(256 * sin(phase * pi / 128)) for phases 1..63. Zero and the
 * quarter-turn value 256 are synthesized so the table remains byte-sized. */
static const byte sine_quarter[63] = {
    6,   12,  18,  25,  31,  37,  43,  49,  56,  62,  68,  74,  80,
    86,  92,  97,  103, 109, 115, 120, 126, 131, 136, 142, 147, 152,
    157, 162, 167, 171, 176, 181, 185, 189, 193, 197, 201, 205, 209,
    212, 216, 219, 222, 225, 228, 231, 234, 236, 238, 241, 243, 244,
    246, 248, 249, 251, 252, 253, 254, 254, 255, 255, 255};

int rift_sin(byte phase) {
  byte half_phase = phase & 127;
  byte folded = half_phase <= 64 ? half_phase : (byte)(128 - half_phase);
  int magnitude;

  if (folded == 0)
    magnitude = 0;
  else if (folded == 64)
    magnitude = 256;
  else
    magnitude = sine_quarter[folded - 1];

  return (phase & 128) ? -magnitude : magnitude;
}

int rift_cos(byte phase) {
  return rift_sin((byte)(phase + 64));
}
