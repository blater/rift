#ifndef RIFT_HOST_SYSTEM_TIME_H
#define RIFT_HOST_SYSTEM_TIME_H

/* src/lib/time.h intentionally owns Rift's sleep API and shadows the system
 * header on the compiler include path. Host runtime code that needs the real
 * C time declarations must step past that local header. The host toolchain is
 * GCC/Clang, both of which support include_next for wrapper headers. */
#include_next <time.h>

#endif /* RIFT_HOST_SYSTEM_TIME_H */
