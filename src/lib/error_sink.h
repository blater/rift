#ifndef RIFT_ERROR_SINK_H
#define RIFT_ERROR_SINK_H

#include "typedefs.h"

void rift_error_text(const char *text);
void rift_error_size(size_t value);
void rift_error_int(int value);
void rift_error_newline(void);

#endif
