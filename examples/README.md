# Rift examples

## Hangman

`hangman.rift` is a structured two-player translation of the original
`hangman.bas`. Player 1 enters a secret word, the screen is cleared, and Player
2 guesses letters while the gallows is drawn:

```sh
./rift examples/hangman.rift
```

The Rift version uses named drawing and input helpers, accepts words of 1-20
letters, compares guesses without regard to case, and ignores repeated or
non-letter guesses.

## ROM clock with fixed-point trig

`clock.rift` renders an analogue clock face and continuously moves its second
hand from the ZX 50 Hz `FRAMES` counter:

```sh
./rift examples/clock.rift
```

Calling `clock_ticks()` selects startup 1 and enables the ROM IM1 service after
CRT initialization. Fixed `sin(byte)` and `cos(byte)` treat `0..255` as one
turn and return signed values scaled by 256, so the example needs no
floating-point library or explicit casts. The program intentionally runs until
it is interrupted.

## ZX colour attributes

`colours.rift` translates a short ZX BASIC colour demonstration. It prints
background-coloured spaces followed by the eight ink colours at normal and
bright intensity:

```sh
./rift examples/colours.rift examples/colours.nex
```

The example exercises nested counter loops, numeric and string output without
newlines, and sticky `paper`, `ink`, and `bright` console attributes.

## Walking cat sprite

`cat_walk.rift` moves a 12-frame 8bpp hardware sprite from right to left,
wrapping it back to the right edge after it reaches the left boundary:

```sh
./rift examples/cat_walk.rift
```

The source sheet is `assets/cat/walk2.png`. It contains twelve 32x32 frames:
the first starts at the left edge and subsequent frames start every 80 pixels,
leaving a 48-pixel gap. Use that 80-pixel stride when regenerating the asset;
a shorter stride progressively clips the right side of later frames. Each exact
crop is reduced to 16x16 with nearest-neighbour sampling. Pixels with alpha
below 128 use transparent palette index `$E3`; the remaining pixels use their
high 3 red, 3 green, and 2 blue bits for the reset RGB332 palette. The packed
frames live in `assets/cat/walk.spr`. The example sets ULA paper and border to
black, clears the existing display with that paper colour, then configures the
sprite transparency index and enables the sprite plane.
