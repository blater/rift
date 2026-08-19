#include "over.h"
#include "plot.h"

void over(byte on) {
  rift_draw_mode = on ? 1 : 0;
}
