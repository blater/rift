#ifndef ROCK_LIGHT_CONSOLE_H
#define ROCK_LIGHT_CONSOLE_H

/* startup=31 deliberately has no stdio streams. Core-only ZX Next programs
 * route runtime diagnostics through the Rock console instead. Formatting is
 * intentionally omitted: these are fatal-path messages, while ordinary Rock
 * output remains fully length-aware through rock_print_bytes(). */
#if defined(__SDCC) && defined(ROCK_ZXN_LIGHT_CORE)
void rock_light_printf(const char *format, ...);

#undef printf
#undef fprintf
#undef fflush
#define printf(...) rock_light_printf(__VA_ARGS__)
#define fprintf(stream, ...) rock_light_printf(__VA_ARGS__)
#define fflush(stream) ((void)0)
#endif

#endif
