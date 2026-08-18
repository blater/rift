//-----------------------------------------------------------------------------
//  RIFT ASSET GENERATOR
//  MIT License
//  Copyright (c) 2024 Paul Passeron
//-----------------------------------------------------------------------------

#include "assets.h"
#include "error.h"
#include "stringview.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SPRITE_PATTERN_BYTES 256
#define SPRITE4_FRAME_BYTES 128
#define SPRITE8_FRAME_BYTES 256
#define SPRITE_PATTERN_CAPACITY 64
#define ZXN_ASSET_FIRST_PAGE 24
#define ZXN_PAGE_BYTES 8192
#define EMIT_ROW_BYTES 16

typedef struct asset_plan_entry {
  ast_t declaration;
  unsigned char *bytes;
  long logical_bytes;
  int frames;
  int patterns;
  int padding;
  int base;
  int is_8bpp;
} asset_plan_entry;

struct asset_plan {
  asset_plan_entry *entries;
  int count;
  int pattern_count;
  long packed_bytes;
};

static void io_error(const char *operation, const char *path) {
  fprintf(stderr, "error: cannot %s asset output '%s': %s\n", operation,
          path, strerror(errno));
  exit(1);
}

static void remove_stale(const char *path) {
  if (remove(path) != 0 && errno != ENOENT) io_error("remove stale", path);
}

static int is_kind(ast_asset_decl asset, const char *kind) {
  size_t length = strlen(kind);
  return asset.kind.lexeme.length == length &&
         memcmp(asset.kind.lexeme.data, kind, length) == 0;
}

static void report_source_change(ast_asset_decl asset) {
  error(asset.path.filename, asset.path.line, asset.path.col,
        "asset source changed while generating build input");
}

static unsigned char *read_snapshot(ast_asset_decl asset) {
  struct stat info;
  if (stat(asset.canonical_path, &info) != 0 || !S_ISREG(info.st_mode) ||
      (long)info.st_size != asset.byte_length) {
    report_source_change(asset);
    return NULL;
  }

  FILE *input = fopen(asset.canonical_path, "rb");
  if (!input) {
    error(asset.path.filename, asset.path.line, asset.path.col,
          "cannot reopen asset source while generating build input");
    return NULL;
  }

  unsigned char *bytes = malloc((size_t)asset.byte_length);
  if (!bytes) {
    fclose(input);
    fprintf(stderr, "error: cannot allocate asset build snapshot\n");
    exit(1);
  }
  size_t actual = fread(bytes, 1, (size_t)asset.byte_length, input);
  int extra = fgetc(input);
  int read_failed = ferror(input);
  int close_failed = fclose(input) != 0;
  if (actual != (size_t)asset.byte_length || extra != EOF || read_failed ||
      close_failed) {
    free(bytes);
    report_source_change(asset);
    return NULL;
  }
  return bytes;
}

asset_plan *asset_generator_plan(ast_array_t declarations) {
  asset_plan *plan = calloc(1, sizeof(*plan));
  if (!plan) {
    fprintf(stderr, "error: cannot allocate asset build plan\n");
    exit(1);
  }
  plan->entries = calloc((size_t)declarations.length,
                         sizeof(*plan->entries));
  if (declarations.length > 0 && !plan->entries) {
    free(plan);
    fprintf(stderr, "error: cannot allocate asset build plan entries\n");
    exit(1);
  }

  for (int i = 0; i < declarations.length; i++) {
    ast_t declaration = declarations.data[i];
    ast_asset_decl *asset = &declaration->data.asset_decl;
    if (!asset->referenced) continue;

    asset_plan_entry *entry = &plan->entries[plan->count++];
    entry->declaration = declaration;
    entry->logical_bytes = asset->byte_length;
    entry->is_8bpp = is_kind(*asset, "sprite8");
    int frame_bytes;
    if (is_kind(*asset, "sprite4"))
      frame_bytes = SPRITE4_FRAME_BYTES;
    else if (entry->is_8bpp)
      frame_bytes = SPRITE8_FRAME_BYTES;
    else {
      error(asset->kind.filename, asset->kind.line, asset->kind.col,
            "unsupported sprite asset format '" SV_Fmt "'",
            SV_Arg(asset->kind.lexeme));
      continue;
    }

    if (asset->byte_length == 0) {
      error(asset->path.filename, asset->path.line, asset->path.col,
            SV_Fmt " asset '" SV_Fmt "' is empty",
            SV_Arg(asset->kind.lexeme), SV_Arg(asset->name.lexeme));
      continue;
    }
    if (asset->byte_length % frame_bytes != 0) {
      error(asset->path.filename, asset->path.line, asset->path.col,
            SV_Fmt " asset '" SV_Fmt
            "' has %ld bytes; expected a multiple of %d",
            SV_Arg(asset->kind.lexeme), SV_Arg(asset->name.lexeme),
            asset->byte_length, frame_bytes);
      continue;
    }

    long frames = asset->byte_length / frame_bytes;
    long patterns = entry->is_8bpp ? frames : (frames + 1) / 2;
    entry->base = plan->pattern_count;
    if (patterns > SPRITE_PATTERN_CAPACITY - plan->pattern_count) {
      error(asset->path.filename, asset->path.line, asset->path.col,
            "sprite pattern capacity exceeded by asset '" SV_Fmt
            "': needs %ld slots in total, maximum is %d",
            SV_Arg(asset->name.lexeme),
            plan->pattern_count + patterns,
            SPRITE_PATTERN_CAPACITY);
      continue;
    }
    entry->frames = (int)frames;
    entry->patterns = (int)patterns;
    entry->padding = entry->patterns * SPRITE_PATTERN_BYTES -
                     (int)asset->byte_length;
    plan->pattern_count += entry->patterns;
    plan->packed_bytes += entry->logical_bytes + entry->padding;
  }

  if (get_error_count() > 0) return plan;
  for (int i = 0; i < plan->count; i++) {
    plan->entries[i].bytes =
        read_snapshot(plan->entries[i].declaration->data.asset_decl);
  }
  return plan;
}

void asset_generator_free(asset_plan *plan) {
  if (!plan) return;
  for (int i = 0; i < plan->count; i++) free(plan->entries[i].bytes);
  free(plan->entries);
  free(plan);
}

int asset_generator_base(const asset_plan *plan, ast_t declaration) {
  if (!plan || !declaration) return -1;
  for (int i = 0; i < plan->count; i++)
    if (plan->entries[i].declaration == declaration)
      return plan->entries[i].base;
  return -1;
}

static void emit_c_bytes(FILE *output, const asset_plan *plan) {
  int row = 0;
  for (int i = 0; i < plan->count; i++) {
    const asset_plan_entry *entry = &plan->entries[i];
    long count = entry->logical_bytes + entry->padding;
    for (long at = 0; at < count; at++) {
      if (row == 0) fprintf(output, "    ");
      unsigned int value = at < entry->logical_bytes ? entry->bytes[at] : 0;
      fprintf(output, "0x%02x,", value);
      row++;
      if (row == EMIT_ROW_BYTES) {
        fputc('\n', output);
        row = 0;
      } else {
        fputc(' ', output);
      }
    }
  }
  if (row != 0) fputc('\n', output);
}

void asset_generator_emit_init(const asset_plan *plan, FILE *output,
                               int target_zxn) {
  if (!plan || plan->count == 0) return;
  if (target_zxn) {
    fprintf(output, "sprite_pattern_upload_banked(%d, 0, %ld, 0);\n",
            ZXN_ASSET_FIRST_PAGE, plan->packed_bytes);
    return;
  }

  fprintf(output, "{\n");
  fprintf(output, "  static const byte bytes[] = {\n");
  emit_c_bytes(output, plan);
  fprintf(output, "  };\n");
  long offset = 0;
  for (int i = 0; i < plan->count; i++) {
    const asset_plan_entry *entry = &plan->entries[i];
    fprintf(output, "  sprite_pattern_upload%d(%d, bytes + %ld, %ld, %d);\n",
            entry->is_8bpp ? 8 : 4, entry->base, offset,
            entry->logical_bytes + entry->padding, entry->frames);
    offset += entry->logical_bytes + entry->padding;
  }
  fprintf(output, "}\n");
}

typedef struct asm_writer {
  FILE *output;
  long offset;
  int row;
  int page;
} asm_writer;

static void asm_end_page(asm_writer *writer) {
  if (writer->row != 0) {
    fputc('\n', writer->output);
    writer->row = 0;
  }
  if (writer->page >= 0)
    fprintf(writer->output, "_rift_assets_page_%d_end:\n", writer->page);
}

static void asm_start_page(asm_writer *writer) {
  writer->page = ZXN_ASSET_FIRST_PAGE +
                 (int)(writer->offset / ZXN_PAGE_BYTES);
  fprintf(writer->output,
          "SECTION PAGE_%d\n"
          "PUBLIC _rift_assets_page_%d_start\n"
          "PUBLIC _rift_assets_page_%d_end\n"
          "_rift_assets_page_%d_start:\n",
          writer->page, writer->page, writer->page, writer->page);
}

static void asm_emit_byte(asm_writer *writer, unsigned int value) {
  if (writer->offset % ZXN_PAGE_BYTES == 0) {
    if (writer->offset != 0) {
      asm_end_page(writer);
      fputc('\n', writer->output);
    }
    asm_start_page(writer);
  }
  if (writer->row == 0) fprintf(writer->output, "  defb ");
  if (writer->row != 0) fputc(',', writer->output);
  fprintf(writer->output, "$%02x", value);
  writer->row++;
  writer->offset++;
  if (writer->row == EMIT_ROW_BYTES) {
    fputc('\n', writer->output);
    writer->row = 0;
  }
}

void asset_generator_emit_asm(const asset_plan *plan, const char *path,
                              int target_zxn) {
  if (!target_zxn || !plan || plan->count == 0) {
    remove_stale(path);
    return;
  }

  FILE *output = fopen(path, "wb");
  if (!output) io_error("create", path);

  asm_writer writer = {.output = output, .offset = 0, .row = 0, .page = -1};
  for (int i = 0; i < plan->count; i++) {
    const asset_plan_entry *entry = &plan->entries[i];
    for (long at = 0; at < entry->logical_bytes; at++)
      asm_emit_byte(&writer, entry->bytes[at]);
    for (int at = 0; at < entry->padding; at++) asm_emit_byte(&writer, 0);
  }
  asm_end_page(&writer);

  if (fclose(output) != 0) io_error("close", path);
}
