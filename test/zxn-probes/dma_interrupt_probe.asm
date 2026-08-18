SECTION code_user

PUBLIC _rift_probe_interrupts_disable
PUBLIC _rift_probe_interrupts_enable
PUBLIC _rift_probe_interrupts_enabled

_rift_probe_interrupts_disable:
  di
  ret

_rift_probe_interrupts_enable:
  ei
  nop
  ret

_rift_probe_interrupts_enabled:
  ld a, i
  ld hl, 0
  ret po
  inc l
  ret
