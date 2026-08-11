SECTION code_user

PUBLIC _rock_probe_interrupts_disable
PUBLIC _rock_probe_interrupts_enable
PUBLIC _rock_probe_interrupts_enabled

_rock_probe_interrupts_disable:
  di
  ret

_rock_probe_interrupts_enable:
  ei
  nop
  ret

_rock_probe_interrupts_enabled:
  ld a, i
  ld hl, 0
  ret po
  inc l
  ret
