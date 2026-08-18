SECTION code_user

PUBLIC _rift_probe_loaded_expanded_page
EXTERN rift_probe_check_page

INCLUDE "nex_page_ramp.inc"

_rift_probe_loaded_expanded_page:
  ld a, $56
  ld bc, $243b
  out (c), a
  ld bc, $253b
  in a, (c)
  push af

  ld a, 96
  ld d, $c3
  call rift_probe_check_page
  jr c, expanded_page_fail

  ld hl, 1
  jr expanded_page_restore

expanded_page_fail:
  ld hl, 0

expanded_page_restore:
  pop af
  push hl
  ld d, a
  ld a, $56
  ld bc, $243b
  out (c), a
  ld a, d
  ld bc, $253b
  out (c), a
  ld a, $56
  ld bc, $243b
  out (c), a
  ld bc, $253b
  in a, (c)
  cp d
  jr z, expanded_page_restore_ok
  pop hl
  ld hl, 0
  ret
expanded_page_restore_ok:
  pop hl
  ret

SECTION PAGE_96
  RIFT_PROBE_RAMP $c3
