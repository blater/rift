SECTION code_user

PUBLIC _rift_probe_loaded_pages
PUBLIC rift_probe_check_page

INCLUDE "nex_page_ramp.inc"

; This leaf uses no writable globals while MMU6 is replaced.  It saves the
; original page on the slot-7 stack, checks every byte of each packaged 8 KiB
; ramp, restores and reads back MMU6, and returns 1/0 in HL.
_rift_probe_loaded_pages:
  ld a, $56
  ld bc, $243b
  out (c), a
  ld bc, $253b
  in a, (c)
  push af

  ld a, 24
  ld d, $00
  call rift_probe_check_page
  jr c, page_fail

  ld a, 25
  ld d, $a7
  call rift_probe_check_page
  jr c, page_fail

  ld a, 95
  ld d, $5a
  call rift_probe_check_page
  jr c, page_fail

  ld hl, 1
  jr page_restore

page_fail:
  ld hl, 0

page_restore:
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
  jr z, page_restore_ok
  pop hl
  ld hl, 0
  ret
page_restore_ok:
  pop hl
  ret

; A = physical 8 KiB page, D = XOR mask for a repeated 0..255 ramp.
; Carry is set on mismatch and clear only after all 8192 bytes compare.
rift_probe_check_page:
  ld e, a
  ld a, $56
  ld bc, $243b
  out (c), a
  ld a, e
  ld bc, $253b
  out (c), a

  ld hl, $c000
  ld bc, $2000
  ld e, 0
check_page_loop:
  ld a, (hl)
  xor d
  cp e
  jr nz, check_page_fail
  inc hl
  inc e
  dec bc
  ld a, b
  or c
  jr nz, check_page_loop
  or a
  ret
check_page_fail:
  scf
  ret

SECTION PAGE_24
  RIFT_PROBE_RAMP $00

SECTION PAGE_25
  RIFT_PROBE_RAMP $a7

SECTION PAGE_95
  RIFT_PROBE_RAMP $5a
