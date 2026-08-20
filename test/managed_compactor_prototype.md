# Routine compactor prototype checkpoint

This checkpoint keeps routine movement private and host-only. Production builds
do not define `RIFT_HEAP_ROUTINE_COMPACTION`; no generator, allocation-pressure,
collection, manifest, or driver path selects it. Normal and pool-free ZX Next
builds therefore contain no compactor or managed relocation symbols.

The C oracle bounds each slice to eight physical headers, one move, and 192
copied bytes. The last C-driven ZX Next experiment measured 2,227 additional
code bytes, nine persistent BSS bytes, and about 57 bytes on the deepest stack
path. Those figures are retained as rejected-prototype costs, not production
claims. No ZX Next pause-time claim is made for this checkpoint.

A fixed-budget naked ZX Next transaction was investigated and rejected. It
could be made to complete the physical move and tail transaction, but the
canonical emulator still failed stable-handle relocation. The incomplete target
routine and all diagnostic instrumentation were removed. ZX Next routine
movement, its <=32-byte stack proof, and <=7,000-T-state timing gate are deferred
until a separately reviewed implementation is correct. Defining the private
prototype feature in an SDCC build is rejected explicitly.

Focused evidence for the retained oracle is `make test-managed-compactor`
(4/4), alongside `make test-managed-handles` (8/8 plus the static-link check)
and `make test-managed-allocator` (11/11).
