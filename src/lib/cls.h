#ifndef RIFT_CLS_H
#define RIFT_CLS_H

/* Clear the screen. On ZXN calls ROM CLS ($0DAF); on the host clears
 * the termbox2 back buffer (or prints a form-feed in the fallback). */
void cls(void);

#endif /* RIFT_CLS_H */
