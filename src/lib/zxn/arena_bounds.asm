; Return the first byte after the linked program's BSS.  C identifiers gain
; an extra leading underscore under SDCC, while z88dk's linker-defined symbol
; must be referenced with its exact spelling here.

SECTION code_compiler

PUBLIC _rift_zxn_arena_link_start
PUBLIC _rift_zxn_arena_link_end
EXTERN __BSS_END_tail
EXTERN REGISTER_SP
EXTERN CRT_STACK_SIZE

_rift_zxn_arena_link_start:
  ld hl,__BSS_END_tail
  ret

_rift_zxn_arena_link_end:
  ld hl,REGISTER_SP - CRT_STACK_SIZE
  ret
