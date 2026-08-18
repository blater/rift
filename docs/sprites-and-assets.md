# Sprite patterns and hardware sprites

Status: implemented

This is the public contract for compiler-loaded sprite patterns and the ZX
Spectrum Next sprite API. The design keeps paths, asset identities, frame
tables, formats, and allocation metadata out of the target program.

## Pattern declarations

A sprite pattern is a compiler-only, file-scope binding:

```rift
SpritePattern invaders :=
  SpritePattern.load("assets/invaders.spr");
SpritePattern player :=
  SpritePattern.load("assets/player.spr", 8);
```

`SpritePattern.load(path)` defaults to 4 bits per pixel. The two-argument
overload accepts only literal `4` or `8`. Both arguments are compile-time
inputs; no loader or format argument reaches the target.

The path is resolved relative to the declaring Rift file. It must name a
regular file within the entry program's source tree. Absolute paths, `..`
escapes, and symlink escapes are rejected.

Patterns may be grouped in a module without creating a runtime module value:

```rift
module Art;
SpritePattern invaders := SpritePattern.load("invaders.spr");
```

```rift
include "Art.rift"

sub main() {
  Sprite alien := Sprite(12);
  alien.frame(Art.invaders, 0);
}
```

An asset-only module emits no type, constructor, array helper, ownership
helper, data, or BSS. A module that also declares runtime fields retains its
ordinary runtime representation.

The former `asset sprite4 name := "path"` syntax is rejected. There is no JSON
manifest and no author, copyright, description, tag, or source metadata.

## Sprite values and API

`Sprite(slot)` constructs a one-byte value containing a hardware slot number:

```rift
Sprite alien := Sprite(12);

alien.position(100, 80);
alien.frame(invaders, 0);
alien.show();

alien.position(101, 80);
alien.frame(invaders, 1);
alien.hide();

Sprite.hideall();
```

Construction does not allocate and has no target constructor function.
Copying a `Sprite` aliases the same hardware slot. It has no destructor or
reference count. `Sprite` remains nominal in Rift: it can be stored in fields,
unions, parameters, returns, and arrays, but only a constructed or copied
`Sprite` value may enter those positions. Generated C backs all of them with
ordinary bytes and the existing byte-array helpers.

The operations are independent:

- `position(x, y)` changes only coordinates.
- `frame(pattern, frame)` changes only the pattern and logical frame.
- `show()` changes only visibility.
- `hide()` changes only visibility.
- `Sprite.hideall()` hides every slot without discarding selected frames.

Slots are `0..127`, X coordinates are `0..319`, and Y coordinates are
`0..255`. Out-of-range literals are compile errors. Host builds reject invalid
dynamic values. ZXN release builds treat dynamic X, Y, and frame ranges as
caller preconditions to avoid linking validation code: X must remain
`0..319`, Y `0..255`, and frame must be within the referenced pattern. Only a
dynamic slot with bit 7 set is safely ignored before hardware or shadow access,
because it could otherwise index beyond the 128-byte table.

The component does not enable the sprite plane or own palette, transparency,
clipping, priority, or display-mode state. The program or screen component
must configure those shared resources before requiring visible output. The raw
NextReg `$15` write in the ZEsarUX smoke test is test-only screen setup, not a
public Sprite operation or runtime ownership claim.

## Raw pattern formats

Both formats contain consecutive raw 16 by 16 frames with no header or
palette:

| Format | Bytes per frame | Pixels per byte | Physical-slot use |
| --- | ---: | ---: | --- |
| 4bpp | 128 | 2, high nibble first | Two logical frames per 256-byte slot |
| 8bpp | 256 | 1 | One logical frame per 256-byte slot |

A file must be non-empty and contain a whole number of frames. An odd 4bpp
frame count receives one zero-filled 128-byte partner. An 8bpp input is never
format-padded.

All referenced 4bpp and 8bpp patterns share the hardware's 64 physical slots.
The compiler asset generator rejects a combined total above 64. A raw file
cannot be safely
introspected for bit depth because 256 bytes could mean two 4bpp frames or one
8bpp frame; the optional loader argument states the intended representation.

## Minimal target state

ZXN sprite attribute registers are write-only. Independent frame and
visibility operations therefore require one shadow byte for attribute 3 in
each hardware slot. The complete target-resident sprite state is exactly 128
bytes of BSS.

That byte retains physical pattern selection, extended-attribute enable, and
visibility. The runtime does not shadow coordinates, logical frames, attribute
4, asset identities, paths, or formats. A frame change writes attribute 3
hidden, writes attribute 4, and restores prior visibility last. This prevents
the renderer from observing a mixed 4/8bpp frame. Position changes are made in
the caller's frame-update window.

The always-linked presentation object is 162 bytes of code plus exactly 128
bytes of writable shadow state. The banked pattern uploader is a separate
92-byte, zero-BSS object selected only when a referenced pattern requires a
startup upload.

Against the empty tiny ZXN profile, an assetless Sprite program adds exactly
335 resident bytes end to end. Referencing patterns adds exactly 111 resident
code bytes: the 92-byte uploader plus 19 bytes of startup integration, with no
further BSS. These are regression assertions, not estimates.

## Compile and build boundary

`SpritePattern` bindings are permitted only at file scope with an exact
`SpritePattern.load` initializer. They cannot be fields, arrays, parameters,
returns, reassigned values, or general runtime expressions. A binding is legal
only as the pattern argument of `Sprite.frame`. Normal lexical shadowing still
applies.

The compiler's separate asset-generation component reads only referenced
bindings, validates their declared format, assigns physical bases, and snapshots
their bytes directly into its generated output. Host bytes and upload calls are
embedded in generated C. ZXN bytes are emitted in one ordinary
`<output>.assets.asm`; no private protocol or external asset-processing stage is
used.

Unreferenced bindings do not enter generated C, generated assembly, or the NEX.
Referenced patterns lower to one-byte physical base constants. Generated target
code contains no path, binding name, handle, frame table, format table,
descriptor, or ownership code.

The build performs one coalesced ZXN startup upload. It maps only MMU1, streams
the packed span from NEX pages to pattern RAM, and restores the previous MMU1
page and caller interrupt state. Host builds use distinct 4bpp and 8bpp upload
hooks to retain validation data; that state is not linked into the target.

## Placement contract

The current ZXN build assigns the exact packed span to provisional `PAGE_24`
and, when required, `PAGE_25`. Sections contain only the bytes required by the
physical pattern layout, so the linker may use an unoccupied page tail. A small
native C verifier reads the map and NEX only; it checks origins, spans, capacity,
RAM profile, bank presence/count, and NEX payload structure, removing a failed
NEX. Byte fidelity is proved by compiler and emulator integration tests rather
than by reopening source assets after the link.

The NEX format stores an occupied bank in a 16 KiB payload. A single 256-byte
physical pattern therefore makes one bank present, but the remaining 16,128
bytes are unused bank capacity rather than emitted asset padding. Moving page
choice into the shared target allocator remains future work; its Rift syntax
has not been agreed.

## Verification

Coverage includes:

1. Both loader arities, literal 4/8 validation, path containment, modules,
   duplicate/case collisions, lexical shadowing, legacy-syntax rejection,
   referenced-only reachability, and absence of module scaffolding.
2. One-byte Sprite construction, copying/aliasing, instance method resolution,
   literal bounds, and invalid dynamic-slot safety.
3. Mixed 4/8bpp layout, odd 4bpp padding, common capacity, direct byte
   emission, deterministic roots, page crossing, reusable tails, native map/NEX
   structure rejection, and emulator-verified bytes.
4. Host position/frame/visibility independence, slot isolation, frame bounds,
   hide-all retention, and 4/8bpp pixel decoding.
5. Exact 162-byte presentation code and 128-byte BSS assertions, a separately
   selected 92-byte uploader, and no other writable sprite symbol.
6. Pinned ZEsarUX evidence for mixed attributes, retained frame state, invalid
   dynamic no-op, slot 127 at `(319,255)`, distinct 4bpp halves, an 8bpp render,
   and a 255-colour capture.
7. Production-uploader evidence across PAGE_24/PAGE_25 proving MMU1 and both
   enabled and disabled interrupt states are restored.

Run the focused checks with:

```sh
make test-asset-language
make test-sprite-runtime
make test-zxn-assets
make test-zxn-size
make test-zxn-sprite
```

The exact 64-slot Invaders scene and gameplay timing remain integration
evidence for the later screen/game stack.
