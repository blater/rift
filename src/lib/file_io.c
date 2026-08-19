#include "fundefs.h"
#include "fundefs_internal.h"

#ifndef __SDCC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef __SDCC
void __read_file_impl(string *out, string filename) {
  (void)filename;
  __rift_make_string(out, "", 0);
}

void write_string_to_file(string s, string filename) {
  (void)s;
  (void)filename;
}

void __get_abs_path_impl(string *out, string path) {
  (void)path;
  __rift_make_string(out, "", 0);
}
#else
void __read_file_impl(string *out, string filename) {
  FILE *f = fopen(string_to_cstr(filename), "r");
  if (f == NULL) {
    printf("Unable to open file \"%s\" for reading: ",
           string_to_cstr(filename));
    perror("");
    exit_rift(1);
  }
  fseek(f, 0, SEEK_END);
  size_t length = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  __rift_make_longlived_string(out, length);
  fread(out->data, 1, length, f);
  out->data[length] = 0;
  fclose(f);
}

void write_string_to_file(string s, string filename) {
  char *fname = string_to_cstr(filename);
  FILE *f = fopen(fname, "wb");
  if (f == NULL) {
    printf("Unable to open file \"%s\" for writing: ",
           string_to_cstr(filename));
    perror("");
    exit_rift(1);
  }
  if (s.data != NULL && s.length > 0) fwrite(s.data, 1, s.length, f);
  fclose(f);
}

void __get_abs_path_impl(string *out, string path) {
  char *abs_path_tmp = realpath(path.data, NULL);
  if (abs_path_tmp == NULL) {
    printf("Could not get absolute path of file '%s'\n", path.data);
    exit(1);
  }
  size_t abs_len = strlen(abs_path_tmp);
  __rift_make_longlived_string(out, abs_len);
  memcpy(out->data, abs_path_tmp, abs_len + 1);
  free(abs_path_tmp);
}
#endif
