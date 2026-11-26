# gcon

An implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language (version 9.5): a translator, a virtual machine and
a graphics display server, built by one `build.sh` with no
`make Configure` step.

gcon is written in C: the translator and tools (`src/`), the runtime
(`vm/`) and the display server (`graphics/`, with C++). It was written
in Go first, then ported to C part by part, each part proved identical
to the Go (see [docs/design.md](docs/design.md), "gcon in C"); the Go
is kept as the tag `go-reference`. The `g` was Go's, the parent; it can
stand for graphics, or general, from here on.

Status: programs pass Icon's own test suite (on the runtime, every one
gcon builds gives iconx's results, or the recorded results that stand
for them), the Icon Program Library runs, and so do C functions loaded
with `loadfunc` and, in part, graphics. See [docs/design.md](docs/design.md)
and [docs/development.md](docs/development.md).

## Layout

| Path | Contents |
|---|---|
| `vm/` | gcon-vm, the runtime: the VM in C, run on every program ([vm/README.md](vm/README.md)) |
| `graphics/` | gcon-graphics, the display server (SDL3 + Blend2D, vendored and pinned in `graphics/deps`) ([graphics/README.md](graphics/README.md)) |
| `src/` | the `gcon` command: translator, driver, `gcon fmt`, `gcon lsp` ([src/README.md](src/README.md)) |
| `lib/ipl` | the Icon Program Library, as source |
| `experiments/` | experiments: Go-style channels on co-expressions, a Tcl/Tk binding |
| `bin/`, `libexec/` | what `build.sh` builds: the checkout is a gcon root |

## Building

    ./build.sh

builds into the checkout, which is then a gcon root, as a Go checkout is
a GOROOT: `bin/gcon` and `libexec/gcon-vm` (need a C compiler) and
`libexec/gcon-graphics` (needs C++ and CMake; without them, programs
cannot open windows). Builds never use the network. Put the root's
`bin` on your `PATH`.

The root holds the Icon Program Library as source, in `lib/ipl`.
`link foo` finds `foo.icn` in the current directory, then in the
directories of `IPATH`, then in the library; `$include` searches the
current directory, `LPATH`, then the library's include files. `gcon env`
shows where it looks. Nothing is written beside your sources: there are
no `.u1`/`.u2` files.

`gcon run file.icn` runs a program; `gcon build file.icn` writes `file`,
an executable that runs the built program, as icont's output runs with
iconx; it no longer needs the sources. `gcon dis` lists a program's VM
code; `gcon fmt` formats sources and `gcon lsp` is a language server.
Programs run on the root's `libexec/gcon-vm` (or on the gcon-vm
`GCONVM` names).

The front end (lexer, parser and name resolution) was forked from
[icontools](https://github.com/phonologus/icontools).

MIT licence; see [LICENSE](LICENSE). The Icon Program Library in
`lib/ipl` is public domain; [NOTICE](NOTICE) reproduces the copyright
notices of the card bitmaps in `lib/ipl/gprocs/cardbits.icn`, as a
precaution. The display server's vendored libraries keep their own
licences (`graphics/`).
