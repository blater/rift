; Enable the ROM's 50 Hz IM1 service for the startup-1 clock component.
;
; The z88dk sdcc_iy library reserves IY and uses IX as the compiler frame
; pointer. Startup 1 enters with interrupts disabled, so it is safe to restore
; the ROM system-variable base in IY before enabling the standard handler.

SECTION code_user

PUBLIC _rift_clock_init
PUBLIC _rift_clock_shutdown

_rift_clock_init:
  di
  ld iy, $5c3a
  im 1
  ei
  ret

_rift_clock_shutdown:
  ret
