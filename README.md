# Rock (Retro·Oriented·Compiler·Kit)

Rock is a statically typed language written in C and targeted at the ZX Next retro system.  
It liberally takes inspiration from C, Rust, Zig, Java, and Modula/Pascal.  

Its designed so that you develop on your pc/laptop and deploy to an emulator or real hardware.
A Rock program can also be directly run locally.


## Language at a glance

Rock provides:

- Scalar types: `int`, `byte`, `word`, `dword`, `float`, `boolean`, `char`, and `string`
- Dynamic and fixed-size arrays
- Records, enums, unions, and modules
- Functions, methods, loops, and `match`
- C and Z80 assembly embed blocks
- A runtime library for strings, arrays, input, graphics, sound, and ZX Spectrum Next access
- Automatic memory allocation, reclaimed automatically when its final reference dies

For example:

```rock
sub main() {
  int[] numbers;

  for i := 1 to 3 {
    append(numbers, i);
  }

  print(toString(length(numbers)));
}
```

#### Embed and call C and Assembly code directly inside rock code
```rock
  @embed c
  int square(int x) { return x * x; }
  @end c

  sub main() {
    print(toString(square(7)));
  }
```

#### enums
```rock
  enum Direction { North, South, East, West }
```

#### Records (with methods)
```rock
  record Point { 
    int x, 
    int y 
  }
  sub Point.move(int dx, int dy) returns Point {
    return { x := this.x + dx, y := this.y + dy };
  }


  sub main() {
    Point start := { x := 2, y := 3 };
    Point end := start.move(1, -1);
  }

```

#### Organise code into modules

```rock
  module Scoreboard;
  int score;

  sub Scoreboard.add(int points) {
    this.score := this.score + points;
  }
```

#### Type-level methods

Use `static sub Type.method(...)` for behaviour owned by a type rather than an
instance. The declaration receives no implicit `this`; callers use the type
name directly.

```rock
  module Sprites;

  static sub Sprites.hideall() {
    // component-wide work
  }

  sub main() {
    Sprites.hideall();
  }
```

Standard opaque interfaces are preloaded by the compiler, so applications do
not repeat a declaration. They expose methods but never their C layout:

```rock
Sprite player;
player.index(1);
player.position(10, 20);
player.show();
Sprite.hideall();
```

Runtime components are selected from resolved calls through
`src/lib/components.manifest`; the same dependency closure drives host and ZX
Next builds.

#### Tagged unions and match
```rock
  union Token {
    // a Token instance is allowed to be *one* of these:
    int Number,
    string Name,
    char Operator,
    End
  }

  Token token := Number(42);

  match token {
    Number: print("a number");
    Name: print("a name");
    Operator: print("an operator");
    End: print("the end");
  }
```


## Requirements

I'll be reducing the pre-requisite requirements in the future (hopefully to nothing!), but for now to compile rock lang code
you need:
- `make`
- `gcc`
- `bash`

Native programs are built with GCC and retro targets (initially ZX Spectrum Next `.nex`) are built with Z88DK, so 
for ZX Spectrum Next builds, also install [Z88DK](https://z88dk.org/) and make `zcc` available on your `PATH`.

## Build the compiler

```bash
make
```

This creates `rockc`, the Rock-to-C compiler.

## Write and run a program

Create `hello.rkr`:

```rock
sub main() {
  print("Hello, Rock!\n");
}
```

Build and run it:

```bash
./rock run hello.rkr
```

The `rock` driver translates the source to C, compiles it, and creates `hello.exe`. The host target, GCC, is the default.

To choose an output name:

```bash
./rock hello.rkr hello
```

To retain the generated C file for inspection:

```bash
./rock hello.rkr --debug
```

## Targets

### Host

Build a native executable with GCC:

```bash
./rock hello.rkr
# or explicitly:
./rock hello.rkr --target=gcc
```

### ZX Spectrum Next

Build a `.nex` program with Z88DK:

```bash
./rock hello.rkr --target=zxn
```

Rock uses Z88DK’s SDCC backend for this target. The generated C and the runtime library are compiled into the `.nex` file.


## Test

Run the host test suite:

```bash
./run_tests.sh
```

Run one test while working on a feature:

```bash
./run_tests.sh test/array_test.rkr
```

Tests are Rock programs in `test/`. Most include `test/Assert.rkr` and print `PASS:` or `FAIL:` markers for the test runner.

## Project layout

```text
src/        Compiler: lexer, parser, type checker, and C generator
src/lib/    Runtime library and target support
test/       Rock regression tests
docs/       Language and implementation notes
wikiroot/   Maintainer knowledge wiki
rock        Build driver for Rock programs
rockc       Generated compiler binary
```

## Development

```bash
make
./run_tests.sh
```

Use `make clean` to remove build output and `rockc`.

The host target has the broadest test coverage. The ZX Spectrum Next target needs Z88DK; compile it separately when a change touches target-specific code. `enum_test.rkr` is currently known to fail on that target because of an SDCC enum syntax incompatibility.

## Further reading

The project wiki in `wikiroot/` describes the compiler pipeline, language syntax, runtime, and target support. Start with `wikiroot/README.md`.
