//-----------------------------------------------------------------------------
//  RIFT ZXN ASSET VERIFIER
//  MIT License
//  Copyright (c) 2024 Paul Passeron
//-----------------------------------------------------------------------------

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEX_HEADER_BYTES 512L
#define NEX_BANK_BYTES 16384L
#define NEX_PRESENT_OFFSET 18
#define NEX_PRESENT_COUNT 112
#define ZXN_PAGE_BYTES 8192U

typedef struct {
  int found;
  unsigned value;
} MapValue;

typedef struct {
  MapValue head[2];
  MapValue size[2];
  MapValue start[2];
  MapValue end[2];
} AssetMap;

static void usage(const char *program) {
  fprintf(stderr,
          "usage: %s --map FILE --nex FILE [--ram-pages=96|224]\n",
          program);
}

static int fail(const char *message) {
  fprintf(stderr, "asset verification failed: %s\n", message);
  return 1;
}

static int fail_path(const char *operation, const char *path) {
  fprintf(stderr, "asset verification failed: cannot %s '%s': %s\n",
          operation, path, strerror(errno));
  return 1;
}

static int parse_map_line(const char *line, const char *symbol,
                          unsigned *value) {
  const char *at = line;
  char *end = NULL;
  unsigned long parsed;
  size_t length = strlen(symbol);

  if (strncmp(at, symbol, length) != 0) return 0;
  at += length;
  if (*at != ' ' && *at != '\t') return 0;
  while (*at == ' ' || *at == '\t') ++at;
  if (*at++ != '=') return 0;
  while (*at == ' ' || *at == '\t') ++at;
  if (*at++ != '$') return 0;
  if ((*at < '0' || *at > '9') && (*at < 'a' || *at > 'f') &&
      (*at < 'A' || *at > 'F'))
    return 0;

  errno = 0;
  parsed = strtoul(at, &end, 16);
  if (errno != 0 || end == at || parsed > UINT_MAX) return 0;
  if (*end != '\0' && *end != '\n' && *end != '\r' && *end != ' ' &&
      *end != '\t')
    return 0;
  *value = (unsigned)parsed;
  return 1;
}

static int set_map_value(MapValue *result, unsigned value, const char *symbol) {
  if (result->found && result->value != value) {
    fprintf(stderr,
            "asset verification failed: conflicting values for map symbol "
            "%s\n",
            symbol);
    return 1;
  }
  result->found = 1;
  result->value = value;
  return 0;
}

static int read_asset_map(const char *map_path, AssetMap *result) {
  FILE *map = fopen(map_path, "r");
  char line[1024];
  unsigned page_index;

  if (map == NULL) return fail_path("open", map_path);
  memset(result, 0, sizeof(*result));
  while (fgets(line, sizeof(line), map) != NULL) {
    for (page_index = 0; page_index < 2; ++page_index) {
      char names[4][64];
      MapValue *values[4] = {&result->head[page_index],
                             &result->size[page_index],
                             &result->start[page_index],
                             &result->end[page_index]};
      unsigned page = 24U + page_index;
      unsigned name_index;

      snprintf(names[0], sizeof(names[0]), "__PAGE_%u_head", page);
      snprintf(names[1], sizeof(names[1]), "__PAGE_%u_size", page);
      snprintf(names[2], sizeof(names[2]), "_rift_assets_page_%u_start", page);
      snprintf(names[3], sizeof(names[3]), "_rift_assets_page_%u_end", page);
      for (name_index = 0; name_index < 4; ++name_index) {
        unsigned value;
        if (parse_map_line(line, names[name_index], &value) &&
            set_map_value(values[name_index], value, names[name_index])) {
          fclose(map);
          return 1;
        }
      }
    }
  }
  if (ferror(map)) {
    fclose(map);
    return fail_path("read", map_path);
  }
  if (fclose(map) != 0) return fail_path("close", map_path);
  return 0;
}

static int verify_page(const AssetMap *map, unsigned page, int required,
                       unsigned *asset_bytes) {
  unsigned page_index = page - 24U;
  MapValue head = map->head[page_index];
  MapValue size = map->size[page_index];
  MapValue start = map->start[page_index];
  MapValue end = map->end[page_index];
  unsigned expected_head = (page & 1U) ? 0xe000U : 0xc000U;
  unsigned asset_symbol_count;

  asset_symbol_count = (unsigned)start.found + (unsigned)end.found;
  if (asset_symbol_count == 0 && !required) {
    *asset_bytes = 0;
    return 0;
  }
  if (!head.found || !size.found || asset_symbol_count != 2) {
    fprintf(stderr,
            "asset verification failed: link map has an incomplete PAGE_%u "
            "asset span\n",
            page);
    return 1;
  }
  if (head.value != expected_head) {
    fprintf(stderr,
            "asset verification failed: PAGE_%u begins at 0x%04X, expected "
            "0x%04X\n",
            page, head.value, expected_head);
    return 1;
  }
  if (start.value != expected_head) {
    fprintf(stderr,
            "asset verification failed: PAGE_%u asset span begins at 0x%04X, "
            "expected 0x%04X\n",
            page, start.value, expected_head);
    return 1;
  }
  if (end.value <= start.value || end.value - start.value > ZXN_PAGE_BYTES) {
    fprintf(stderr,
            "asset verification failed: PAGE_%u asset span is not within one "
            "8 KiB page\n",
            page);
    return 1;
  }
  if (size.value > ZXN_PAGE_BYTES || size.value < end.value - head.value) {
    fprintf(stderr,
            "asset verification failed: PAGE_%u section size does not contain "
            "its asset span within 8 KiB\n",
            page);
    return 1;
  }
  *asset_bytes = end.value - start.value;
  return 0;
}

static int verify_nex(const char *nex_path, unsigned ram_pages,
                      int has_page25) {
  FILE *nex = fopen(nex_path, "rb");
  unsigned char header[NEX_HEADER_BYTES];
  unsigned present_count = 0;
  unsigned index;
  unsigned expected_ram = ram_pages == 96 ? 0U : 1U;
  long length;

  if (nex == NULL) return fail_path("open", nex_path);
  if (fread(header, 1, sizeof(header), nex) != sizeof(header)) {
    fclose(nex);
    return fail("NEX is shorter than its 512-byte header");
  }
  if (memcmp(header, "Next", 4) != 0) {
    fclose(nex);
    return fail("bad NEX signature");
  }
  if ((unsigned)header[8] != expected_ram) {
    fclose(nex);
    fprintf(stderr,
            "asset verification failed: NEX RAM_Required=%u, expected %u\n",
            (unsigned)header[8], expected_ram);
    return 1;
  }
  for (index = 0; index < NEX_PRESENT_COUNT; ++index) {
    if (header[NEX_PRESENT_OFFSET + index] != 0) ++present_count;
  }
  if (present_count != (unsigned)header[9]) {
    fclose(nex);
    return fail("NEX bank count differs from its presence table");
  }
  if (ram_pages == 96) {
    for (index = 48; index < NEX_PRESENT_COUNT; ++index) {
      if (header[NEX_PRESENT_OFFSET + index] != 0) {
        fclose(nex);
        return fail("NEX contains a bank outside the 96-page RAM profile");
      }
    }
  }
  if (header[NEX_PRESENT_OFFSET + 12] == 0) {
    fclose(nex);
    return fail("NEX omits asset bank 12 for PAGE_24/25");
  }
  if (fseek(nex, 0, SEEK_END) != 0) {
    fclose(nex);
    return fail_path("seek", nex_path);
  }
  length = ftell(nex);
  if (length < 0) {
    fclose(nex);
    return fail_path("measure", nex_path);
  }
  if (length != NEX_HEADER_BYTES + (long)present_count * NEX_BANK_BYTES) {
    fclose(nex);
    return fail("NEX payload length differs from its bank table");
  }
  if (fclose(nex) != 0) return fail_path("close", nex_path);

  printf("asset build verified: PAGE_24%s, %u NEX payload bank%s, "
         "%u-page RAM profile\n",
         has_page25 ? "/25" : "", present_count,
         present_count == 1 ? "" : "s", ram_pages);
  return 0;
}

int main(int argc, char **argv) {
  const char *map_path = NULL;
  const char *nex_path = NULL;
  unsigned ram_pages = 96;
  unsigned page24_bytes;
  unsigned page25_bytes;
  AssetMap map;
  int index;

  for (index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--map") == 0 && index + 1 < argc) {
      map_path = argv[++index];
    } else if (strcmp(argv[index], "--nex") == 0 && index + 1 < argc) {
      nex_path = argv[++index];
    } else if (strncmp(argv[index], "--ram-pages=", 12) == 0) {
      const char *value = argv[index] + 12;
      if (strcmp(value, "96") == 0)
        ram_pages = 96;
      else if (strcmp(value, "224") == 0)
        ram_pages = 224;
      else {
        usage(argv[0]);
        return 2;
      }
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (map_path == NULL || nex_path == NULL) {
    usage(argv[0]);
    return 2;
  }
  if (read_asset_map(map_path, &map) ||
      verify_page(&map, 24, 1, &page24_bytes) ||
      verify_page(&map, 25, 0, &page25_bytes))
    return 1;
  if (page25_bytes != 0 && page24_bytes != ZXN_PAGE_BYTES)
    return fail("PAGE_25 is present before the PAGE_24 asset span is full");
  return verify_nex(nex_path, ram_pages, page25_bytes != 0);
}
