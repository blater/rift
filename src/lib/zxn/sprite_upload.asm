SECTION code_user

PUBLIC _sprite_pattern_upload_banked

; void sprite_pattern_upload_banked(byte source_page, word source_offset,
;                                   word byte_count, byte pattern_base)
;
; MMU1 ($2000-$3fff) is below Rift's $5b00 code origin and below neither this
; routine nor the hardware stack.  The uploader maps one 8 KiB NEX page at a
; time, streams directly to pattern port $xx5b, and retains no descriptor or
; writable scratch storage.  IFF2 and the original MMU1 page are restored.
_sprite_pattern_upload_banked:
  push ix
  ld ix, 0
  add ix, sp

  ld a, i
  di
  push af

  ld a, $51
  ld bc, $243b
  out (c), a
  ld bc, $253b
  in a, (c)
  push af

  ld a, (ix+9)
  ld bc, $303b
  out (c), a

  ld b, (ix+4)
  ld e, (ix+5)
  ld a, (ix+6)
  add a, $20
  ld d, a
  ld l, (ix+7)
  ld h, (ix+8)

  ld a, b
  nextreg $51, a
  ld c, $5b

upload_loop:
  ld a, h
  or l
  jr z, upload_done
  ld a, (de)
  out (c), a
  inc de
  dec hl
  ld a, d
  cp $40
  jr nz, upload_loop
  ld d, $20
  inc b
  ld a, b
  nextreg $51, a
  jr upload_loop

upload_done:
  pop af
  nextreg $51, a
  pop af
  jp po, upload_leave_disabled
  ei
upload_leave_disabled:
  pop ix
  ret
