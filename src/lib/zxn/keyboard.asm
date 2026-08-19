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
        xor $FF        ; flip the bits. so 1 is pressed
        and $1F        ; 00011111b -  are any bits on?
	jr z, nopress  ; no keys pressed

; start checking the 5 key press bits one by one...
; key5
	rr a           ; rotate right most bit into carry
	jr nc, not5    ; is the rightmost bit zero
	ld e,(hl)      ; it is one, so key must be pressed
	inc e          ; inc key strength
	jr z, key4     ; if we have gone over 255 dont store
	ld (hl),e      ; else store key strength
	jr key4
not5:
	ld (hl), 0     ; set to not pressed

; --------------------------
key4:
	inc hl 		; next key
	rr a		; rotate keypress bits right
	jr nc, not4	; if rightmost bit 0 then nothing pressed
	ld e,(hl)	; else key pressed, load the current press strength
	inc e 		; increment key press strength (use for debounce)
	jr z, key3	; if we went over 255 then dont store it
	ld (hl),e 	; store press strength
	jr key3
not4:
	ld (hl), 0 	; not pressed. Turn key off

; --------------------------
key3:
	inc hl
	rr a
	jr nc, not3
	ld e,(hl)
	inc e
	jr z, key2
	ld (hl),e
	jr key2
not3:
	ld (hl), 0

; --------------------------
key2:
	inc hl
	rr a
	jr nc, not2
	ld e,(hl)
	inc e
	jr z, key1
	ld (hl),e
	jr key1
not2:
	ld (hl), 0

; --------------------------
key1:
	inc hl
	rr a
	jr nc, not1
	ld e,(hl)
	inc e
	ret z
	ld (hl),e
	ret
not1:
	ld (hl), 0
	ret

nopress:
	ld (hl), a  ; set key press indicators for the half row to zero
		    ; using A as reg assign is faster (A is implicitly 0)
	inc hl
	ld (hl), a
	inc hl
	ld (hl), a
	inc hl
	ld (hl), a
	inc hl
	ld (hl), a
	inc hl
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
