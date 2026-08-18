#ifndef RIFT_POLYLINE_H
#define RIFT_POLYLINE_H

#include "typedefs.h"

/* Raster polyline on the ZX ULA screen.
 *
 * polyline(xs, ys) draws connected line segments between parallel arrays
 * of X and Y byte coordinates. For N points it emits N-1 draw() calls
 * using consecutive pairs. Arrays with length < 2 are no-ops; a length
 * mismatch trims to the shorter of the two.
 *
 * Thin wrapper over draw() — inherits the dispatcher, the H/V fast
 * paths, the Bresenham general fallback, and the current draw mode.
 */

void polyline(__internal_dynamic_array_t xs, __internal_dynamic_array_t ys);

#endif /* RIFT_POLYLINE_H */
