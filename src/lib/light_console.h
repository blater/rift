#ifndef RIFT_LIGHT_CONSOLE_H
#define RIFT_LIGHT_CONSOLE_H

/* startup=31 deliberately has no stdio streams. Core-only ZX Next programs
 * route runtime diagnostics through the Rift console instead. Formatting is
 * intentionally omitted: these are fatal-path messages, while ordinary Rift
 * output remains fully length-aware through rift_print_bytes(). */
#if defined(__SDCC) && defined(RIFT_ZXN_LIGHT_CORE)
void rift_light_printf(const char *format, ...);

#undef printf
#undef fprintf
#undef fflush
#define printf(...) rift_light_printf(__VA_ARGS__)
#define fprintf(stream, ...) rift_light_printf(__VA_ARGS__)
#define fflush(stream) ((void)0)
#endif

#endif
