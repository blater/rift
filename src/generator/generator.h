/*****************************************************
 * RIFT GENERATOR HEADER
 * MIT License
 * Copyright (c) 2024 Paul Passeron
 *****************************************************/

#ifndef GENERATOR_H
#define GENERATOR_H

#include "ast.h"
#include "component_manifest.h"

typedef enum {
  TARGET_HOST,
  TARGET_ZXN
} target_t;

typedef struct generator_t generator_t;

typedef struct generator_options {
  target_t target;
  int auto_cast;
  int zxn_test;
  int select_all_components;
  int semantic_plan;
} generator_options;

generator_t *new_generator(char *filename, const char *output_base,
                           component_manifest *components,
                           generator_options options);
void kill_generator(generator_t *g);

void transpile(generator_t *g, ast_t program);

#endif // GENERATOR_H
