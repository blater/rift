SECTION code_user

; ZX Next small-object allocator hit path.
;
; The C runtime owns the buddy core, diagnostics, and explicit collection.
; These entry points only accelerate a live 32-byte block (26-byte payload)
; moving between its magazine and the caller.  All misses and all other
; sizes tail-call the C implementation.  The block layout is fixed by
; pools.h on SDCC:
;   +0 size (uint16), +2 refcount (uint16), +4 next_free (uint16), +6 payload
;
; Calls use SDCC's ordinary one-word stack argument ABI, not a private ABI.
; The caller removes its argument after return, so preserve it on the stack.

PUBLIC _rock_longlived_alloc
PUBLIC _rock_longlived_free

EXTERN _rock_longlived_alloc_slow
EXTERN _rock_longlived_free_slow
EXTERN _rock_ll_base
EXTERN _rock_ll_cap
EXTERN _rock_ll_live_bytes
EXTERN _rock_ll_peak_live_bytes
EXTERN _rock_ll_magazine_free_bytes
EXTERN _rock_magazine_heads
EXTERN _rock_magazine_counts

DEFC ROCK_HEADER_SIZE = 6
DEFC ROCK_CLASS1_PAYLOAD = 26
DEFC ROCK_CLASS1_TOTAL = 32
DEFC ROCK_RC_MAGAZINE_LO = 0xfd
DEFC ROCK_RC_MAGAZINE_HI = 0xff
DEFC ROCK_OFFSET_NONE_LO = 0xff
DEFC ROCK_OFFSET_NONE_HI = 0xff

; Extract a normal C argument into HL while leaving the original argument
; under the return address exactly as the caller expects.
MACRO LOAD_STACK_WORD
  pop de
  pop hl
  push hl
  push de
ENDM

; Return via the C slow path without changing its conventional stack frame.
_rock_longlived_alloc:
  LOAD_STACK_WORD
  ld a,h
  or a  ; If A == 0, the Z (zero) flag is set.
  jp nz,_rock_longlived_alloc_slow
  ld a,l
  cp 11
  jp c,_rock_longlived_alloc_slow
  cp 27
  jp nc,_rock_longlived_alloc_slow

  ; A class-1 magazine hit: HL becomes its block header.
  ld hl,(_rock_magazine_heads + 2)
  ld a,h
  and l
  inc a
  jp z,_rock_longlived_alloc_slow
  ld de,(_rock_ll_base)
  add hl,de
  push hl

  ; Pop the next offset from header + 4 into the class-1 head.
  ld de,4
  add hl,de
  ld e,(hl)
  inc hl
  ld d,(hl)
  ld (_rock_magazine_heads + 2),de
  ld hl,(_rock_magazine_counts + 2)
  dec (hl)
  jr nz,alloc_count_done
  inc hl
  dec (hl)
alloc_count_done:

  ; The block leaves the deferred-free accounting.
  ld hl,(_rock_ll_magazine_free_bytes)
  ld de,ROCK_CLASS1_PAYLOAD
  or a
  sbc hl,de
  ld (_rock_ll_magazine_free_bytes),hl

  pop hl
  ; refcount = 1, next_free = NONE.
  inc hl
  inc hl
  ld (hl),1
  inc hl
  ld (hl),0
  inc hl
  ld (hl),ROCK_OFFSET_NONE_LO
  inc hl
  ld (hl),ROCK_OFFSET_NONE_HI

  ; Exact live and peak diagnostics remain valid in release builds.
  push hl
  ld de,(_rock_ll_live_bytes)
  ld bc,ROCK_CLASS1_TOTAL
  ex de,hl
  add hl,bc
  ld (_rock_ll_live_bytes),hl
  ld de,(_rock_ll_peak_live_bytes)
  or a
  sbc hl,de
  jr c,alloc_peak_done
  jr z,alloc_peak_done
  add hl,de
  ld (_rock_ll_peak_live_bytes),hl
alloc_peak_done:
  ; Restore header + 5 and advance once to the payload.
  pop hl
  inc hl
  ret

_rock_longlived_free:
  LOAD_STACK_WORD
  ; Work from the header. Range-check before reading it: an invalid pointer
  ; must remain the C runtime's diagnostic, never an assembly dereference.
  ld de,-ROCK_HEADER_SIZE
  add hl,de
  push hl
  ld de,(_rock_ll_base)
  or a
  sbc hl,de
  jp c,free_slow
  push hl
  ld de,(_rock_ll_cap)
  or a
  sbc hl,de
  pop hl
  jp nc,free_slow
  ld de,(_rock_ll_base)
  add hl,de

  ; Anything not a live 32-byte pool block is handled by C, including static,
  ; double-free, non-small, and full-magazine cases.
  ld a,(hl)
  cp ROCK_CLASS1_PAYLOAD
  jp nz,free_slow
  inc hl
  ld a,(hl)
  or a
  jp nz,free_slow
  inc hl
  inc hl
  ld a,(hl)
  cp 2
  jp nc,free_slow
  inc hl
  ld a,(hl)
  or a
  jp nz,free_slow
  pop hl

  ; Convert the already-checked header address to its pool-relative offset.
  ld de,(_rock_ll_base)
  or a
  sbc hl,de

  ; A full magazine must flow into C so its buddy return/coalesce remains
  ; canonical.  Class 1 has exactly two deferred slots.
  ld de,(_rock_magazine_counts + 2)
  ld a,d
  or a
  jp nz,free_slow_from_offset
  ld a,e
  cp 2
  jp nc,free_slow_from_offset

  ; HL is the header offset.  Materialise the header pointer in DE and save
  ; the offset for the intrusive next link.
  push hl
  ld de,(_rock_ll_base)
  add hl,de
  ex de,hl
  pop hl
  push hl

  ; refcount = MAGAZINE and next_free = previous class-1 head.
  ex de,hl
  inc hl
  inc hl
  ld (hl),ROCK_RC_MAGAZINE_LO
  inc hl
  ld (hl),ROCK_RC_MAGAZINE_HI
  inc hl
  ld de,(_rock_magazine_heads + 2)
  ld a,(de)
  ld (hl),a
  inc hl
  inc de
  ld a,(de)
  ld (hl),a
  pop hl
  ld (_rock_magazine_heads + 2),hl

  ; Count and free-byte accounting.
  ld hl,(_rock_magazine_counts + 2)
  inc hl
  ld (_rock_magazine_counts + 2),hl
  ld hl,(_rock_ll_magazine_free_bytes)
  ld de,ROCK_CLASS1_PAYLOAD
  add hl,de
  ld (_rock_ll_magazine_free_bytes),hl
  ld hl,(_rock_ll_live_bytes)
  ld de,ROCK_CLASS1_TOTAL
  or a
  sbc hl,de
  ld (_rock_ll_live_bytes),hl
  ret

; Restore the original payload from the header/offset state before falling
; through to the C public-contract implementation.
free_slow_from_offset:
  ld de,(_rock_ll_base)
  add hl,de
  push hl
free_slow:
  pop hl
  ld de,ROCK_HEADER_SIZE
  add hl,de
  jp _rock_longlived_free_slow
