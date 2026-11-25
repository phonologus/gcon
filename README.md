# gcon-vm

The virtual machine for gcon (Go-based Icon), in C. gcon's toolchain, in
Go, translates Icon 9.5 programs to gcon's bytecode; this VM runs that
bytecode. It is the counterpart of iconx to gcon's icont, as
[gcon-graphics](https://github.com/phonologus/gcon-graphics) is gcon's
display server: C builds are kept out of the Go repository.

**Status: design and measurement.** Nothing runs Icon yet. See
[docs/design.md](docs/design.md) for why the VM is in C and how it is
built, and `experiments/spike` for the measurement behind it.

## Building

    ./build.sh

needs only a C compiler (C11, with the computed-goto extension that GCC
and Clang provide), and leaves `bin/gcon-vm`.

## Licence

MIT; see [LICENSE](LICENSE). Code taken from the reference
implementation of Icon (public domain) is marked where it is used.
