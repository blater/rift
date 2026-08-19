// Keep code above the ULA and reserve a 2 KiB hardware stack at the top of
// RAM. NEX does not need BASIC's system-variable area at 0x5b00-0x5fff, so
// starting there gains 1,280 resident bytes for the bounded Rift pools. The
// driver reads the z88dk link map and rejects any image whose BSS would enter
// the reserved stack range.
// The previous 24500/24481 layout placed the stack inside a larger NEX image.
#pragma output CRT_ORG_CODE = 23296
#pragma output REGISTER_SP = 65368

// halt program on exit, no need to play well with basic
#pragma output CRT_ON_EXIT = 0x10001

// eliminate the malloc heap
#pragma output CLIB_MALLOC_HEAP_SIZE = 0

// eliminate the stdio heap (no files can be opened)
#pragma output CLIB_STDIO_HEAP_SIZE = 0
#pragma output CLIB_FOPEN_MAX = -1
