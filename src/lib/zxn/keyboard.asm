SECTION code_user

; Rift RTL - ZX Spectrum keyboard scanner
; Reads the 8x5 key matrix and maintains a per-key press-strength buffer.
; C-callable entry point: _scan_keyboard
; The 40-byte press buffer is exposed as ZK_BUFFER for keyboard.c.

PUBLIC _scan_keyboard
PUBLIC _ZK_BUFFER

; -----------------+---+---+---+---+---+
; BIT              | 4 | 3 | 2 | 1 | 0 |
; ------+----------+---+---+---+---+---+
; C= $FE|          |   |   |   |   |   |
; B= $F7|%11110111 | 5 | 4 | 3 | 2 | 1 |
;    $EF|%11101111 | 6 | 7 | 8 | 9 | 0 |
;    $FB|%11111011 | T | R | E | W | Q |
;    $DF|%11011111 | Y | U | I | O | P |
;    $FD|%11111101 | G | F | D | S | A |
;    $BF|%10111111 | H | J | K | L |ENT|
;    $FE|%11111110 | V | C | X | Z |SHF|
;    $7F|%01111111 | B | N | M |SYM|SPC|
; ------+----------+---|---+---+---+---+

_scan_keyboard:
	ld hl, keybuffer
	ld c, 0xFE  ; LSB port address

	ld b, keys_5_to_1
	call scan_half_row

	ld b, keys_6_to_0
	call scan_half_row

	ld b, keys_T_to_Q
	call scan_half_row

	ld b, keys_Y_to_P
	call scan_half_row

	ld b, keys_G_to_A
	call scan_half_row

	ld b, keys_H_to_ENT
	call scan_half_row

	ld b, keys_V_to_SHF
	call scan_half_row

	ld b, keys_B_to_SPC
	call scan_half_row
	ret

scan_half_row:
	in a, (c)
	xor $FF
	and $1F

	; KEY_* IDs follow the labels above from bit 4 to bit 0. Move bit 4
	; into bit 7 so each SLA presents the next key in carry.
	add a, a
	add a, a
	add a, a
	ld d, 5

scan_key:
	sla a
	jr nc, key_not_pressed
	ld e, (hl)
	inc e
	jr z, next_key       ; saturate press strength at 255
	ld (hl), e
	jr next_key

key_not_pressed:
	ld (hl), 0

next_key:
	inc hl               ; always leave HL at the following buffer slot
	dec d
	jr nz, scan_key
	ret

; key row IO addresses
DEFC keys_5_to_1   = $F7
DEFC keys_6_to_0   = $EF
DEFC keys_T_to_Q   = $FB
DEFC keys_Y_to_P   = $DF
DEFC keys_G_to_A   = $FD
DEFC keys_H_to_ENT = $BF
DEFC keys_V_to_SHF = $FE
DEFC keys_B_to_SPC = $7F

; 40-byte key press buffer, one byte per key. Indexed by KEY_* constants
; defined in src/lib/keyboard.h. ZK_BUFFER is the C-visible alias.
keybuffer:
	defb 0,0,0,0,0  ; 5-1
	defb 0,0,0,0,0  ; 6-0
	defb 0,0,0,0,0  ; T-Q
	defb 0,0,0,0,0  ; Y-P
	defb 0,0,0,0,0  ; G-A
	defb 0,0,0,0,0  ; H-ENT
	defb 0,0,0,0,0  ; V-SHF
	defb 0,0,0,0,0  ; B-SPC
DEFC _ZK_BUFFER = keybuffer
