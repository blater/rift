// Keep code above the ULA and the hardware stack below UDG memory. Code begins
// at 0x6000 so the full RTL and its ZXN BSS pools fit below the 0xff58 stack.
// The previous 24500/24481 layout placed the stack inside a larger NEX image.
#pragma output CRT_ORG_CODE = 24576
#pragma output REGISTER_SP = 65368

// halt program on exit, no need to play well with basic
#pragma output CRT_ON_EXIT = 0x10001

// eliminate the malloc heap
//#pragma output CLIB_MALLOC_HEAP_SIZE = 0

// eliminate the stdio heap (no files can be opened)
#pragma output CLIB_STDIO_HEAP_SIZE = 0

// eliminate unused printf converters
// placed used converters here, empty string if none (e.g. "%c %s %u %d")
#pragma printf = "%c %s %u %d"    
