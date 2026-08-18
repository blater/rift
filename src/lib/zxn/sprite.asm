SECTION code_user

PUBLIC _rift__im_L6_Sprite_L8_position_A2
PUBLIC _rift__im_L6_Sprite_L6_frame4_A2
PUBLIC _rift__im_L6_Sprite_L6_frame8_A2
PUBLIC _rift__im_L6_Sprite_L4_show_A0
PUBLIC _rift__im_L6_Sprite_L4_hide_A0
PUBLIC _rift__tm_L6_Sprite_L7_hideall_A0
PUBLIC _rift_sprite_attr3_shadow

; SDCC_IY caller-clean arguments begin at SP+2:
;   position: slot, word x, word y
;   frame:    slot, pattern_base, logical_frame

; Position changes only coordinates.  Reject an invalid dynamic receiver
; before selecting a hardware slot.
_rift__im_L6_Sprite_L8_position_A2:
  ld hl, 2
  add hl, sp
  ld a, (hl)
  bit 7, a
  ret nz
  nextreg $34, a
  inc hl
  ld a, (hl)
  nextreg $35, a
  inc hl
  ld b, (hl)
  inc hl
  ld a, (hl)
  nextreg $36, a
  ld a, b
  and 1
  nextreg $37, a
  ret

; Frame changes only the selected pattern and format.  Attribute 3 is written
; hidden first, followed by attribute 4, then the prior visibility is restored
; last.  The single shadow byte per slot is necessary because attribute 3 is
; write-only hardware state.
_rift__im_L6_Sprite_L6_frame4_A2:
  ld hl, 2
  add hl, sp
  ld a, (hl)
  bit 7, a
  ret nz
  ld b, a
  inc hl
  ld c, (hl)
  inc hl
  ld a, (hl)
  ld d, a
  and 1
  rrca
  rrca
  or $80
  ld e, a
  ld a, d
  srl a
  add a, c
  jr frame_apply

_rift__im_L6_Sprite_L6_frame8_A2:
  ld hl, 2
  add hl, sp
  ld a, (hl)
  bit 7, a
  ret nz
  ld b, a
  inc hl
  ld c, (hl)
  inc hl
  ld a, (hl)
  add a, c
  ld e, 0

frame_apply:
  or $40
  ld d, a
  ld c, b
  ld b, 0
  ld hl, _rift_sprite_attr3_shadow
  add hl, bc
  ld a, (hl)
  and $80
  or d
  ld (hl), a
  ld b, a

  ld a, c
  nextreg $34, a
  ld a, d
  nextreg $38, a
  ld a, e
  nextreg $39, a
  ld a, b
  nextreg $38, a
  ret

; Show and hide only change the visibility bit in the attribute-3 shadow.
_rift__im_L6_Sprite_L4_show_A0:
  ld d, $80
  jr visibility_apply

_rift__im_L6_Sprite_L4_hide_A0:
  ld d, 0

visibility_apply:
  ld hl, 2
  add hl, sp
  ld a, (hl)
  bit 7, a
  ret nz
  ld c, a
  ld b, 0
  ld hl, _rift_sprite_attr3_shadow
  add hl, bc
  ld a, (hl)
  and $7f
  or d
  ld (hl), a
  ld d, a
  ld a, c
  nextreg $34, a
  ld a, d
  nextreg $38, a
  ret

; Clear visibility in both the hardware and shadow while preserving every
; slot's selected pattern and attribute-4 enable bit.
_rift__tm_L6_Sprite_L7_hideall_A0:
  xor a
  nextreg $34, a
  ld hl, _rift_sprite_attr3_shadow
  ld b, 128
hideall_loop:
  ld a, (hl)
  and $7f
  ld (hl), a
  nextreg $78, a
  inc hl
  djnz hideall_loop
  ret

SECTION bss_user

; The entire target-resident sprite state: one byte for write-only attribute 3
; per hardware slot.  Do not add coordinates, frame IDs, assets, or formats.
_rift_sprite_attr3_shadow:
  defs 128
