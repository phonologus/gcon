# gcon-vm

The virtual machine for gcon (Go-based Icon), in C. gcon's toolchain, in
Go, translates Icon 9.5 programs to gcon's bytecode; this VM runs that
bytecode. It is the counterpart of iconx to gcon's icont, as
[gcon-graphics](https://github.com/phonologus/gcon-graphics) is gcon's
display server: C builds are kept out of the Go repository.

**Status: the loader.** gcon-vm reads and checks a program gcon has
built, and lists it (`gcon-vm -dis prog`, identical to `gcon dis
prog`); nothing runs Icon yet. See [docs/design.md](docs/design.md) for
why the VM is in C and how it is built, and `experiments/spike` for the
measurement behind it.

## Building

    ./build.sh

needs only a C compiler (C11, with the computed-goto extension that GCC
and Clang provide), and leaves `bin/gcon-vm`. `$CC` and `$CFLAGS` are
used if set.

## Testing

The tests compare gcon-vm with gcon, so they need Go and gcon's source
(`$GCONSRC`, by default `../gcon` beside this repository); building
gcon-vm needs neither.

    tests/dis.sh tests/values.icn ../icon/tests/general ../icon/ipl/progs

builds each program with gcon and checks that gcon-vm's listing is
identical to gcon's.

    tests/damage.sh tests/values.icn

checks every truncation and every changed byte of a built program:
gcon-vm, built with the address and undefined-behaviour sanitizers,
must report each exactly as gcon does.

`src/gen_tables.c` is generated from gcon's sources by
`tools/gentables.sh`: the names of gcon's builtins, and which characters
are printable (for images).

## Licence

MIT; see [LICENSE](LICENSE). Code taken from the reference
implementation of Icon (public domain) is marked where it is used.
