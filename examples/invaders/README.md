# Rift Invaders — Game and Platform Plan

**Status:** Planned; typed 4/8bpp patterns, one-byte Sprite values, the
minimal-state hardware API, asset layout, and verified startup upload are
implemented. The shared page allocator and the screen, Layer 2, Tilemap,
palette, frame-timing, DMA, and audio components remain to be delivered.

## Outcome

Build a complete Space Invaders-style game in Rift which is useful in two
ways:

1. it is a polished, playable ZX Spectrum Next demo; and
2. it is an integration client for the graphics, asset, timing, input, music,
   and sound-effect APIs a Rift game developer needs.

The final game must not contain `@embed c`, `@embed asm`, raw `next_reg_set`,
`mmu_set`, `peek`, or `poke` calls. Those escape hatches are useful while
developing the runtime, but accepting them in the demo would hide missing Rift
APIs. Hardware access belongs in `src/lib/` components with host behaviour,
auto-link rules, tests, and target evidence.

## Definition of done

- A title screen, attract loop, one- or two-player start choice, gameplay,
  player death, level completion, game over, scoring, lives, and high-score
  table all run from Rift code.
- The gameplay screen uses native Next planes: 320x256 8bpp Layer 2, 40x32
  Tilemap, and 4bpp hardware sprites. The ULA layer is disabled during the
  game and restored on exit.
- All 55 invaders, the player, UFO, bullets, and transient explosion frames
  fit within the 128 hardware-sprite attribute slots.
- The main loop advances exactly once per video frame without using
  `sleep()`. Game speed and music remain stable at every CPU turbo setting.
- Title music and the invader march play without blocking. Shot, explosion,
  UFO, player-death, and bonus sounds can interrupt or duck assigned channels
  without corrupting music state.
- Keyboard controls work through the existing matrix scanner. Control reads
  are isolated behind Rift functions so a future joystick component can be
  added without changing game logic.
- Assets are declared and compiled into the target by the Rift build; no target
  build depends on runtime file I/O or an unmanaged pointer into a temporary
  MMU mapping.
- No gameplay-frame allocation is required. Fixed arrays hold entity state,
  and build/link reports prove that resident memory, Layer 2 banks, Tilemap
  memory, assets, runtime pools, and the 2 KiB stack reservation either do not
  overlap or share a page only through an explicitly ordered lifetime reuse.
- `make`, the host suite, focused logic tests, a ZXN build, and ZEsarUX
  execution/visual evidence pass.

## Chosen Next display architecture

The game uses the full 320x256 display. Plane order is configured symbolically
as **Sprites -> ULA/Tilemap -> Layer 2**; the video RTL owns the exact `$15`
encoding.

<!-- markdownlint-disable MD013 -->

| Plane | Invaders use | Why |
| --- | --- | --- |
| Hardware sprites | 55 invaders, player, UFO, bullets, explosion frames | Moving objects update attributes instead of erasing/redrawing pixels. Pattern assets may be 4bpp or 8bpp and share 64 physical pattern slots. |
| 40x32 Tilemap, 2-byte entries | HUD, text glyphs, ground line, four destructible shields | 8x8 cells suit text and shield chunks; individual shield definitions can be changed without redrawing the playfield. |
| Layer 2, 320x256 8bpp | Title/logo art, star field, playfield backdrop, colour effects | Full-screen, full-colour pixels with no ULA colour clash. Mostly static content avoids unnecessary 80 KiB frame copies. |
| ULA | Disabled during title/game; restored at shutdown | Existing ULA drawing remains available for simpler programs but is not the right renderer for this demo. |

<!-- markdownlint-enable MD013 -->

Layer 2 uses a single visible buffer for the first playable release. Moving
objects are sprites and changing shield cells are Tilemap data, so a full
double buffer is not required. The Layer 2 API still supports an optional
shadow page and hardware page flip for other games and for future animated
screens.

### Hardware budgets

**Sprite attributes:** reserve slots 0-54 for invaders, 55 for the player,
56 for the UFO, 57 for the player shot, 58-60 for enemy shots, and 61-63 for
short-lived effects. An alien being destroyed can temporarily change its own
pattern, so explosions do not normally consume another attribute. The first
release stays within 64 of the available 128 slots.

**Sprite patterns:** Invaders normally loads 4bpp patterns. Six alien animation
frames, player, UFO, three projectile frames, two explosions, and UI effects
use fewer than 32 logical frames. A 4bpp frame is 128 bytes and two frames share
one 256-byte physical slot; an odd frame count consumes a zero-filled partner
half. An 8bpp frame is 256 bytes and consumes one complete slot. Mixed 4bpp and
8bpp assets share the hardware's 64 physical slots, and the asset generator's
total,
not a hand-maintained estimate, is authoritative.

### Implemented sprite and pattern language

Patterns are compiler-only bindings with an explicit type and a type-level
loader:

```rift
SpritePattern invaders :=
  SpritePattern.load("assets/invaders.spr");
SpritePattern player :=
  SpritePattern.load("assets/player.spr", 8);
```

`SpritePattern.load(path)` is the one-argument overload and means
`SpritePattern.load(path, 4)`. The two-argument overload accepts only the
compile-time integer literals `4` and `8`, meaning bits per pixel. The path is
also a compile-time literal. Both forms snapshot and validate the file during
compilation and produce no runtime loader, path, descriptor, format byte, or
pattern object. Rift already resolves type-level method overloads by arity.

The raw format is deliberately deterministic: 4bpp inputs must contain a
whole number of 128-byte frames and 8bpp inputs a whole number of 256-byte
frames. A headerless byte stream cannot be introspected safely because 256
bytes could be either two 4bpp frames or one 8bpp frame. Self-describing source
formats may be added later, but the second argument still states the requested
hardware representation.

Each hardware slot is wrapped by a one-byte value object, while position,
frame, and visibility remain separate operations:

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

`Sprite(12)` is value construction, not allocation: its complete target
representation is the one-byte hardware slot number. It has no destructor or
reference count. Copying the value aliases the same hardware slot. Its argument
is a byte; a literal outside `0..127` is a compile error. A dynamic value has
the same target-side caller precondition, but every method must reject its high
bit before indexing the shadow so invalid input cannot corrupt memory or force
the general error runtime to link.

`position` changes only coordinates, `frame` changes only the pattern image,
and `show`/`hide` change only visibility. `hideall` remains type-level because
it affects every slot. Instance-method lowering passes the slot byte directly,
so the object adds no call indirection over an explicit slot argument.

The ZXN attribute registers are write-only, so independent `frame`, `show`,
and `hide` cannot truthfully be implemented without remembering attribute 3.
The target therefore owns exactly one shadow byte per hardware slot: 128 bytes
of fixed BSS. That byte contains the physical pattern selection, extended
attribute enable, and visibility. Coordinates, logical frame numbers, asset
identities, formats, paths, and attribute 4 are not shadowed. `position` writes
hardware only; `frame` updates the one shadow byte and hardware; `show` and
`hide` change its visibility bit; and `hideall` clears the visibility bit in
hardware and every shadow entry without discarding the selected frames. No
heap-backed sprite object or full attribute record is permitted.

Frame changes must write a temporarily hidden attribute 3, update attribute 4,
and restore the prior visibility last, preventing the renderer from observing
a mixed 4/8bpp frame. Position updates occur during the documented frame-update
window. Tests must prove that each operation preserves every attribute it does
not own.

This API intentionally replaces the previous `asset sprite4 name := "path"`
declaration and five-argument
`Sprite.show(slot, x, y, pattern, frame)` call. The compatibility break is
accepted before the demo adopts the API. The always-linked presentation object
is 162 code bytes plus exactly 128 bytes of BSS; an assetless linked Sprite
feature adds 335 resident bytes over the empty tiny profile. Referenced-pattern
startup conditionally adds 111 code bytes—92 for the otherwise-unlinked banked
uploader and 19 for its startup call—and no further BSS.

The Sprite component does not enable the sprite plane or own palette,
transparency, clip, priority, or display-mode state. The raw NextReg `$15`
write in the ZEsarUX smoke test is test-only screen setup; the eventual Invaders
screen component must own that global policy.

**Tilemap bank 5:** Rift code starts at `$5B00`. The usual `$6000` Tilemap
sample layout would overwrite generated code, so the demo instead reserves
`$4000-$49FF` for 1,280 two-byte map entries and `$4A00-$5AFF` for at most 136
4bpp tile definitions. The ULA is disabled while these ranges are used. The
asset/placement tooling must validate the final addresses rather than rely on
this plan alone.

**Layer 2:** 320x256 8bpp consumes five contiguous 16 KiB banks (ten 8 KiB
pages), initially based at 16 KiB bank 9. That candidate is pages 18-27 and
therefore collides with the sprite asset generator's current provisional
PAGE_24/25
source placement. The final page range must be allocated by one target layout
manifest and must not be hard-coded in Rift source.

**Resident game state:** use fixed `byte`/`word` arrays and parallel arrays in
hot paths. Do not allocate strings, records, or dynamic arrays during a frame.
The current ZXN runtime provides a 1 KiB bump pool and 6 KiB long-lived pool;
the demo must pass the exact target memory profile and link-map stack guard.

**Bank switching limitation:** the sprite uploader is safe and restores MMU1
and the caller's interrupt state, but `mmu_set()` is not a general safe banked
memory abstraction. A normal Rift NEX can place code and BSS in MMU slot 6
(`$C000-$DFFF`), so application code which maps another page there can unmap
the instructions or globals it is currently using. The game must not call
`mmu_set()` directly. General mutable banked access still needs the safe
bank-window service; startup-only sprite upload does not need a resident bank
descriptor or generic runtime allocator.

## Asset placement flexibility required

The current asset pipeline is deliberately honest but fixed: the compiler's
asset-generation component emits an exact referenced-only span beginning at
PAGE_24, uses PAGE_25 when necessary, and a native post-link verifier rejects a
conflicting final map. It cannot yet choose a different free page. Invaders
cannot combine that fixed choice with its candidate Layer 2 layout.

The smallest sufficient fix is one deterministic **build-time target layout
allocator**. It must add no allocator, registry, asset ID, path, descriptor, or
page table to the target image. Automatic placement remains the default, but a
Rift author must be able to constrain it in Rift source.

### Asset-placement syntax remains undecided

Only the language requirement is agreed: automatic placement remains the
default, and an author who needs control can select the packed asset source
location in Rift source rather than JSON or a driver-only manifest. The exact
spelling has not been agreed and must not be frozen during implementation.

No public syntax for soft preferences, allowed ranges, named reservations, or
lifetime reuse has been agreed. The build still needs internal fixed and
component claims to allocate a complete target safely, but those do not
justify exposing a placement language. Start with the smallest author control
that is agreed later.

User placement can constrain the result but cannot bypass profile, alignment,
overlap, or post-link checks. Omitting it preserves automatic deterministic
placement. There is deliberately no manual `free`, runtime page handle, or
mapped pointer.

### Required build contract

1. After component and asset reachability is known, every selected producer
   emits a claim to one shared layout input. A claim carries only fields the
   allocator uses: owner/role, exact byte count, byte alignment, contiguity,
   and lifetime. Source paths and descriptive metadata are not placement
   inputs.
2. Fixed reservations describe the selected 96- or 224-page machine profile,
   loader/system pages, resident linker ranges, stack, and fixed Bank 5
   windows. Flexible claims include the packed sprite source, Layer 2 buffers,
   Tilemap definitions that live outside fixed Bank 5, music, and future
   banked data.
3. Once its syntax is agreed, an explicit source constraint participates in
   the same allocator and reports the conflicting owner when it cannot be met.
4. Allocation runs once, before target sources and PAGE sections are emitted.
   It allocates exact byte spans within 8 KiB PAGE sections, preserving usable
   first/last-page space unless a claim requires page alignment. It rejects
   out-of-profile or overlapping claims, satisfies alignment and contiguity,
   minimises newly occupied 16 KiB NEX banks, then fragmentation, then uses the
   lowest page and offset as the deterministic tie-breaker.
5. The chosen page and offset are lowered into generated constants and linker
   section names. The existing sprite startup call still contains only source
   page, offset, byte count, and destination pattern base. No placement data
   remains as target-resident tables.
6. The post-link pass reconciles every claim against the map, NEX presence
   table, exact payload bytes, selected RAM profile, and all other claims. A
   mismatch deletes the failed NEX.

The driver therefore becomes a small two-phase build:

```text
compile and resolve reachability
  -> measure referenced content and collect claims
  -> allocate all target pages once
  -> emit placement-specific C/assembly/linker sections
  -> link
  -> reconcile map and NEX against the same allocation
```

### Lifetime reuse

Sprite source pages are read once during startup and are dead after upload.
Layer 2 pages are runtime-writable. Reusing the same physical pages could save
an entire NEX bank, but it is legal only when the shared init graph proves:

```text
NEX loads sprite source
  -> sprite upload completes
  -> screen/Layer 2 may clear or overwrite the reused pages
```

The current compiler calls component init hooks before its generated sprite
startup upload, so an init hook that touches Layer 2 memory would destroy an
overlaid sprite source too early. Before lifetime overlap is enabled, asset
upload must become an explicit node in the init dependency graph, and the
allocator must require a proven happens-before edge. Until then, startup-read
and runtime-write claims remain disjoint. Guessing from component names or
relying on user call order is not acceptable.

The first allocator release need only relocate disjoint exact spans and honour
the single author constraint eventually agreed. Lifetime reuse is a second
gate after init ordering is explicit; it must not delay basic user-controlled
relocation.

### Placement tests

- Force the same sprite build around different reserved-page sets and prove
  relocation changes only generated page constants/sections, never resident
  BSS or asset descriptors.
- Combine the exact Invaders claims: ten contiguous Layer 2 pages, fixed Bank 5
  Tilemap windows, referenced sprite bytes, and representative audio data.
- Cover non-zero page offsets, reusable first/last-page space, 8 KiB page
  boundaries, 16 KiB bank pairing, alignment, exact-fit, fragmentation,
  profile limits, deterministic tie-breaking, and unsatisfiable layouts.
- Exercise automatic placement, the agreed author constraint, and conflicting
  user constraints with source-located errors.
- Corrupt each map/NEX/claim field and require post-link rejection.
- When lifetime reuse is implemented, prove sprite upload before Layer 2
  overwrite and render both the uploaded sprite and the reused framebuffer.
- Assert that changing placement adds zero target BSS/data and introduces no
  runtime layout symbols or metadata.

This placement work does not require JSON, a runtime asset API, or a general
heap allocator in banked RAM. Page and bank numbers are available to authors
who need control, but are never mandatory and compile entirely out of the
target image.

## Current capability and gap matrix

The source was checked on 2026-08-14. The canonical wiki root was unavailable,
so its older sprite/asset pages must not override the implemented source and
tests summarised here.

<!-- markdownlint-disable MD013 -->

| Capability | Status today | Plan |
| --- | --- | --- |
| Rift control flow, functions, records, modules, fixed arrays, arithmetic | Exists | Implement all rules, entity state, collision, scoring, and tests in Rift. Avoid target enums until the known SDCC enum failure is fixed. |
| Simultaneous keyboard input | Exists: `scan_keyboard`, `key_pressed` | Default keys: `O`/`P` move, `SPACE` fires, `ENTER` starts, `H` pauses. |
| ULA pixel, readback, lines, shapes, fill, text, colour, clear | Exists | Useful fallback/debug surface, but not the final renderer. |
| Random numbers | Exists | Seed once; use only for enemy shot selection and cosmetic stars. |
| Generic NextReg and MMU access | Exists | Foundation for RTL internals; deliberately forbidden in demo code. |
| Safe bank switching, page reservations, and bounded bank windows | **Missing; raw `mmu_set` is unsafe for game code** | [Banked memory ticket](../../plans/todo-zxn-banked-memory.md). |
| Beeper tone and millisecond sleep | Exists but unsuitable | Both are 3.5 MHz calibrated and blocking. They cannot drive a turbo-safe game or mixed audio. |
| Asset layout and sprite upload | **Exists with fixed placement** | `SpritePattern.load` emits referenced mixed 4/8bpp inputs into the same 64 physical slots and verifies NEX structure natively. Replace provisional PAGE_24/25 with the shared allocator above. |
| Reusable zxnDMA transfers | **Missing** | [DMA ticket](../../plans/todo-zxn-dma.md). |
| Screen-mode/layer lifecycle and plane priority | **Missing** | [Video modes ticket](../../plans/todo-zxn-video-modes.md). |
| Palette upload, selection, and safe edit state | **Missing** | [Palette ticket](../../plans/todo-zxn-palettes.md). |
| Layer 2 draw, clear, blit, scroll, and optional page flip | **Missing** | [Layer 2 ticket](../../plans/todo-zxn-layer2.md). |
| Hardware sprite pattern/attribute API | **Exists** | The one-byte `Sprite(slot)` value has independent instance `position`, `frame`, `show`, and `hide` methods plus type-level `hideall`. Only the required 128-byte attribute-3 shadow is writable; screen ownership remains separate. |
| Tilemap setup, tile upload, cell update, and scroll | **Missing** | [Tilemap ticket](../../plans/todo-zxn-tilemap.md). |
| Video-frame wait/counter independent of CPU speed | **Missing** | [Frame timing ticket](../../plans/todo-zxn-frame-timing.md). |
| Turbo Sound AY register/channel driver | **Missing** | [AY driver ticket](../../plans/todo-zxn-ay.md). |
| Non-blocking music playback | **Missing** | [Music player ticket](../../plans/todo-zxn-music.md). |
| Non-blocking prioritised sound effects | **Missing** | [SFX player ticket](../../plans/todo-zxn-sfx.md). |

<!-- markdownlint-enable MD013 -->

## Planned source layout

Only this plan is created now. Implementation should grow into this layout:

```text
demo/invaders/
  README.md                 this contract and delivery plan
  main.rift                  entry point and state transitions
  constants.rift             dimensions, slots, timings, score values
  state.rift                 fixed game-state records and arrays
  controls.rift              action mapping over keyboard APIs
  formation.rift             invader movement, animation, descent, firing
  collision.rift             AABB/projectile/shield collision
  render.rift                plane setup and calls into graphics RTL
  audio.rift                 music/SFX event mapping
  Assets.rift                file-scope Rift asset declarations
  assets/                   raw hardware-ready binary inputs
  test/logic_test.rift       deterministic host game-rule regression
  test/collision_test.rift   collision and shield-damage regression
```

`main.rift` includes the other Rift files. Keep hardware constants out of these
files: application constants live in `constants.rift`, while register numbers,
bit packing, ports, and bank-window rules remain private to the RTL.

## Game model

### State

- Five rows by eleven columns of aliens. Sprite slot equals alien index, so an
  `alien_alive[55]` byte array is enough; screen position is derived from the
  formation origin, row, column, and current animation frame.
- One player with x position, lives, respawn timer, and one active shot.
- Three enemy-shot slots with x/y, active flag, and projectile pattern.
- One UFO with active flag, x/direction, spawn timer, and bonus value.
- Four shields, each represented by unique 8x8 Tilemap definition cells plus a
  resident logical damage mask. A hit changes only the affected definition.
- Scalar score, high score, level, direction, animation counter, march period,
  and next movement/fire ticks.

All counters are frame based. Use `word` for score/ticks where 8 bits are too
small and use wrap-safe comparisons for long-running counters.

### Fixed tick order

Each frame performs the same sequence:

1. `video_wait_frame()` and advance the monotonic frame counter.
2. Scan the keyboard once and derive press/held/release game actions.
3. Apply pause/start/quit state transitions.
4. Move the player and update player shot.
5. On the scheduled march tick, move/animate the whole formation; reverse and
   descend when its live bounds touch the playfield edge.
6. Select/update enemy shots and UFO state.
7. Resolve shot/alien, shot/UFO, shot/shield, shot/shot, player/shield, and
   invader/ground collisions in a deterministic order.
8. Update only dirty sprite attributes, shield tile definitions, HUD cells,
   and Layer 2 effects.
9. Call `music_tick()` and `sfx_tick()` once.

No step sleeps, waits for audio, loads a file, or allocates a managed value.

### Collision policy

Use CPU AABBs for sprites. The hardware sprite collision flag identifies only
that some collision occurred, not the pair, so it is not the game-rule oracle.
Projectile-to-shield checks first consult the resident shield mask, then apply
a small deterministic damage stamp and upload only the changed 8x8 definition.
This keeps collisions testable on host and rendering efficient on target.

### Timing and difficulty

- Target gameplay cadence is 50 ticks/second. The frame API reports the active
  refresh rate so 60 Hz configurations can accumulate a 50 Hz logical tick
  without changing game or music speed.
- Formation movement period decreases as live-alien count falls, reproducing
  the accelerating march without relying on CPU workload.
- Animation toggles on movement ticks. Audio march events are emitted from the
  same logical tick, keeping motion and sound locked together.
- Player input and projectile animation still update every display frame.

## Delivery sequence

### I0 — deterministic Rift game logic

Create the planned Rift files, state arrays, fixed-tick rules, collisions, and
host tests. Use a test renderer/audio adapter made of Rift functions. This can
start before the native graphics stack and proves the language can express the
game without per-frame allocation.

**Exit gate:** scripted ticks cover edge reversal, alien kill/scoring, shield
damage, player death/respawn, level reset, and game over on the host.

### I1 — target foundations

Deliver the shared page allocator, safe general bank windows, DMA, video-mode
lifecycle, palettes, and frame timing. The `SpritePattern.load` overloads,
mixed 4/8bpp packing, one-byte `Sprite(slot)` value, separate
position/frame/visibility operations, exact 128-byte shadow, and verified
startup upload already exist.

**Exit gate:** a small Rift target fixture switches from ULA to 320x256 Layer
2, loads palettes/assets, waits for frames at multiple turbo speeds, and
restores the prior display without memory overlap.

### I2 — native visual slice

Deliver Layer 2 and Tilemap, then integrate the existing sprite component into
a title screen and one static gameplay frame before movement, projectiles,
shield erosion, and HUD updates.

**Exit gate:** all 55 animated aliens move as hardware sprites over a Layer 2
backdrop; Tilemap score and shield changes are visible in ZEsarUX; no raw
hardware access appears in `demo/invaders`.

### I3 — native audio

Deliver AY, music, and SFX components. Integrate title music, the four-note
march, firing/explosion/UFO/death effects, and channel priority rules.

**Exit gate:** ten minutes of scripted gameplay has stable tempo, no stuck AY
notes, no blocking audio call, and clean silence/display restoration on exit.

### I4 — complete and harden

Add menus, attract loop, two-player turn switching, levels/difficulty,
high-score-in-session, polish, memory/cycle measurements, and final target
evidence. Persistent high scores are explicitly deferred until ZXN file
storage has its own safe design; the current target sets the stdio heap to
zero.

**Exit gate:** the definition of done above is satisfied and all linked TODO
tickets required by the game are closed.

## Verification plan

For each RTL ticket, follow the repository RTL convention: one public header,
one C shim, optional ZXN assembly, a host implementation, builtin registration,
auto-link detection, and a focused `.rift` regression.

The sprite revision gate additionally requires:

- language tests for both `SpritePattern.load` arities, 4/8-only literal
  validation, one-byte `Sprite(slot)` construction, copying/aliasing, instance
  method resolution, and rejection of literal slots outside `0..127`;
- asset-generator tests for mixed 4/8bpp ordering, odd 4bpp pairing, 8bpp
  whole-slot
  allocation, the shared 64-slot limit, reachability, and deterministic output;
- host tests proving position, frame, visibility, slot isolation, copying, and
  `hideall` preserve unrelated state;
- target object/map assertions for exactly 128 bytes of sprite writable state
  and no full attribute, coordinate, asset, or format tables; and
- ZEsarUX attribute and rendered evidence showing independent movement and
  animation for both formats, visibility preservation across frame changes,
  and `hideall` followed by `show` retaining the chosen frame.

Final integration evidence:

```sh
make
./run_tests.sh
make test-zxn-assets test-zxn-sprite
./run_tests.sh demo/invaders/test/logic_test.rift
./run_tests.sh demo/invaders/test/collision_test.rift
./rift demo/invaders/main.rift demo/invaders/invaders
tools/rift-emu run --target zxn demo/invaders/invaders.nex
tools/rift-emu inspect --target zxn demo/invaders/invaders.nex
```

Add target fixtures to `tools/test-zxn` only after each component has a
deterministic assertion channel. Visual evidence must include title, gameplay,
shield damage, and game-over frames. Timing evidence must report frame misses
and representative T-state budgets at the 3.5 MHz baseline as well as normal
28 MHz play.

## Deliberate non-goals for the first game

- Legacy software sprites or ULA XOR sprites.
- Per-frame full Layer 2 copies when hardware sprites/dirty tiles suffice.
- Custom IM2 game logic; frame-driven polling is simpler and preserves the ROM
  input contract. A later interrupt audio backend can build on the same public
  API.
- Hardware sprite collision as the only collision oracle.
- Runtime asset loading from SD card.
- Persistent high scores, networking, mouse input, or a level editor.
- Embedding target assembly in the demo to bypass an unfinished ticket.

## Repository references

- `docs/sprites-and-assets.md` — implemented syntax, format, target costs, and
  focused evidence.
- `wikiroot/pages/runtime-library/rtl-overview.md` — RTL component contract.
- `wikiroot/pages/targets/zxn-hardware.md` — Next display/audio overview.
- `wikiroot/pages/targets/zxn/zxn-sprites.md` — sprite attributes/patterns.
- `wikiroot/pages/targets/zxn/zxn-layer2.md` — 320x256 bank layout.
- `wikiroot/pages/targets/zxn/zxn-tilemap.md` — Tilemap memory and attributes.
- `wikiroot/pages/targets/zxn/zxn-palette.md` — per-plane palettes.
- `wikiroot/pages/targets/zxn/zxn-sound.md` — Turbo Sound AY hardware.
- `wikiroot/pages/targets/zxn/zxn-dma.md` — zxnDMA programming.
- `plans/todo-zxn-banked-memory.md` — the safe page ownership and MMU-window
  subset required by this game.
- `plans/banked-data-plan.md` — target page-manifest work that asset placement
  must share rather than duplicate.
