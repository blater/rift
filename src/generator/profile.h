#ifndef GENERATOR_PROFILE_H
#define GENERATOR_PROFILE_H

#include "generator.h"

typedef struct generator_profile_analysis {
  int eligible;
  int uses_stdout;
  int simple_stdout;
} generator_profile_analysis;

generator_profile_analysis generator_analyse_profile(target_t target,
                                                      ast_t program);

#endif
