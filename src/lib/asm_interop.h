#ifndef RIFT_ASM_INTEROP_H
#define RIFT_ASM_INTEROP_H

#include "typedefs.h"

// Direct memory access functions for Z80 assembly interoperability
// On ZXN target: Direct pointer dereference
// On host target: Stubbed (return 0 / no-op)

byte peek(word address);
void poke(word address, byte val);

#endif // RIFT_ASM_INTEROP_H
