#ifndef RIFT_CLS_H
#define RIFT_CLS_H

/* Clear the screen. On ZXN clears the ULA bitmap and attribute memory using
 * the current permanent attribute; on the host clears the termbox2 back
 * buffer (or prints a form-feed in the fallback). */
void cls(void);

#endif /* RIFT_CLS_H */
