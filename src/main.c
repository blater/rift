#include "lib/alloc.h"
#include "error.h"
#include "generator.h"
#include "lexer.h"
#include "parser.h"
#include "token.h"
#include "typechecker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #include "generator.h"

void usage(char *name) {
  printf("Usage:\n");
  printf("\t%s <input file> [output file] [options]\n", name);
  printf("Options:\n");
  printf("\t--target=zxn\t\tCompile for ZX Spectrum Next\n");
  printf("\t--auto-cast\t\tWrap int args with (byte)/(word)/(dword) when callee param is narrower\n");
  printf("\t--zxn-test\t\tEmit ZXN emulator test result markers\n");
  // printf("\t%s [flags] <input file> [output file] [flags]\n", name);
  // printf("Possible flags:\n");
  // printf("\t-t:\t\tPrints the ast\n");
  // printf("\t-l:\t\tPrints the list of lexemes\n");
}

int main(int argc, char *argv[]) {
  init_compiler_stack();
  if (argc < 2) {
    usage(argv[0]);
    exit(1);
  }
  char *input = NULL;
  char *output = NULL;
  // int print_tree = 0;
  int print_lexer = 0;
  int target_zxn = 0;
  int auto_cast = 0;
  int zxn_test = 0;

  for (int i = 1; i < argc; i++) {
    char *arg = argv[i];

    if (*arg == '-') {
      // This is a flag
      if (strlen(arg) == 1) {
        printf("Unexpected empty flag !\n");
        usage(argv[0]);
        exit(1);
      }
      // Check for --target=zxn
      else if (strncmp(arg, "--target=", 9) == 0) {
        if (strcmp(arg + 9, "zxn") == 0) {
          target_zxn = 1;
        } else if (strcmp(arg + 9, "gcc") != 0) {
          printf("defaulting target to gcc");
        }
      }
      else if (strcmp(arg, "--auto-cast") == 0) {
        auto_cast = 1;
      }
      else if (strcmp(arg, "--zxn-test") == 0) {
        zxn_test = 1;
      }
      //  else if (*(arg + 1) == 't' && !print_tree)
      //   print_tree = 1;
      else if (*(arg + 1) == 'l' && !print_lexer)
        print_lexer = 1;
      else {
        printf("Unknown flag '%s'!\n", arg + 1);
        usage(argv[0]);
        exit(1);
      }

    } else if (input == NULL)
      input = arg;
    else if (output == NULL) {
      output = arg;
    } else {
      printf("Unexpected argument '%s' !\n", arg);
      usage(argv[0]);
      exit(1);
    }
  }
  if (input == NULL) {
    printf("Expected input !\n");
    usage(argv[0]);
    exit(1);
  }
  if (output == NULL)
    output = "out";

  input = get_abs_path(input, NULL);

  lexer_t l = new_lexer(input);
  token_array_t prog = lex_program(&l);
  // if (print_lexer)
  //   print_token_array(prog);
  parser_t p = new_parser(prog);
  p.source = l.data;
  p.source_length = l.length;
  parse_program(&p);

  // ADR-0003 §9.4: structural-acyclicity check on user-defined types.
  // Runs before generation; cycles are reported via error() and bail out
  // through the get_error_count() check below.
  check_acyclic_types(p.prog);
  if (get_error_count() > 0) {
    kill_compiler_stack();
    return 1;
  }

  char *cout = allocate_compiler_persistent(strlen(output) + 3);
  sprintf(cout, "%s.c", output);
  generator_t g = new_generator(cout);
  if (target_zxn) g.target = TARGET_ZXN;
  if (zxn_test && !target_zxn) {
    printf("--zxn-test requires --target=zxn\n");
    kill_compiler_stack();
    return 1;
  }
  g.zxn_test = zxn_test;
  g.auto_cast = auto_cast;
  transpile(&g, p.prog);
  kill_generator(g);

  // If there were any compilation errors, exit after cleanup
  if (get_error_count() > 0) {
    kill_compiler_stack();
    return 1;
  }

  // clean up
  kill_compiler_stack();
  return 0;
}
