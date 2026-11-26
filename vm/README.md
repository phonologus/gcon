# gcon-vm

The virtual machine for gcon, in C. gcon translates Icon 9.5 programs
to its bytecode; this VM runs that bytecode. It is the counterpart of
iconx to gcon's icont.

**Status: running programs.** `gcon-vm prog args...` runs a program gcon
has built (`gcon build`), with iconx's results on gcon's program tests
and Icon's own suite; `gcon-vm -dis prog` lists one (`gcon dis` calls
it). Memory is managed by a precise mark-and-sweep collector. It has
every Icon 9.5 function, including `loadfunc`: C functions written to
9.5's `icall.h` (`include/icall.h`) recompile unchanged. Graphics goes
to gcon's display server (`../graphics`). See
[docs/design.md](docs/design.md) for why the VM is in C and how it is
built.

## Building

    ./build.sh

needs only a C compiler (C11, with the computed-goto extension that GCC
and Clang provide), and leaves `bin/gcon-vm`. `$CC` and `$CFLAGS` are
used if set.

## Using it with gcon

gcon-vm is part of gcon, and gcon's `build.sh` installs it in the
root's `libexec`. To install it in another root:

    tools/install.sh [gcon-root]

copies `bin/gcon-vm` to that root's `libexec` (by default this
checkout's), where `gcon run`, and so every program gcon builds, finds
it and hands programs to it. `GCONVM` overrides the choice (a path to a
gcon-vm); `gcon env` shows it. gcon-vm can also be run directly on a
built program:

    gcon-vm [-progname name] program [argument ...]

## Testing

The tests need gcon (built from this checkout, `..`, or `$GCON`) and
the Icon sources beside the checkout (`../../icon`); building gcon-vm
needs neither.

    tests/programs.sh

runs gcon's program tests (`tests/programs`, expected results recorded
from iconx).

    tests/conformance.sh

runs Icon's own suite (`../../icon/tests/general`) as its `Test-icon`
does and compares with iconx's output; `tests/conformance.txt` lists
the tests that pass.

    tests/expected.sh

runs the reference tests and the IPL's programs (`../icon/tests/general`,
`../icon/ipl/progs`) and compares output, messages and exit status with
their recorded runs in `tests/expected/` (recorded from gcon's Go VM
while it was the reference; see `tests/expected/README`).

    tests/damage.sh tests/programs/arith.icn

checks every truncation and every changed byte of a built program:
gcon-vm, built with the address and undefined-behaviour sanitizers,
must report each exactly as the root's gcon-vm does.

    tests/cfuncs.sh

builds the IPL's C functions (`../icon/ipl/cfuncs`) against
`include/icall.h` and checks them through `loadfunc`, and compares with
iconx when it can.

    tests/graphics.sh

draws on a hidden canvas through the display server and compares with
`tests/graphics.std`.

    tests/window.sh

opens real windows (briefly; it needs a display), with keystrokes and
clicks scripted through the display server, and compares each
program's output with the expected (`tests/window/`).

    tests/ref.sh

runs icon-sdl's reference drawing programs and compares their images
with unmodified Icon's under X11 (`tests/ref/expected`), within the
tolerances of the SDL backend (`tests/ref/tolerance`).

    tests/iconx.sh [prog.icn ...]

runs graphics programs (by default the ones gcon ships, `../lib/ipl`)
under gcon and under icon-sdl's iconx, built beside gcon's checkout, and
compares their output and every canvas pixel for pixel; without
icon-sdl it does nothing.
`tests/bench.sh` times micro-benchmarks on gcon-vm and iconx.

`src/gen_tables.c` holds the names of gcon's operations and built-ins;
it was generated from the Go reference and is maintained by hand.
`src/unicode.c`, which characters are printable (for images), is
generated from the Unicode Character Database by `tools/mkunicode.sh
VERSION` (with `tools/mkunicode.c`), run when moving to a new Unicode
version.

## Licence

MIT; see [LICENSE](LICENSE). Code taken from the reference
implementation of Icon (public domain) is marked where it is used.
