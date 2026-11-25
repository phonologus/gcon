# gcon

An implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language (version 9.5) in Go: a translator and a virtual
machine in a single `go build`, with no `make Configure` step.

Status: early, but the language runs: `gcon run file.icn` passes 84 of
the 89 tests of Icon's own test suite (of the rest, two need C external
functions, two differ by design in Unicode csets, and one depends on
iconx's memory regions and on set order, which gcon randomises). See [docs/design.md](docs/design.md) and
[docs/development.md](docs/development.md).

Build gcon into the repository, which is then a gcon root, as a Go
checkout is a GOROOT:

    go build -o bin/gcon ./cmd/gcon

and put its `bin` on your `PATH`. The root holds the Icon Program
Library as source, in `lib/ipl`. `link foo` finds `foo.icn` in the
current directory, then in the directories of `IPATH`, then in the
library; `$include` searches the current directory, `LPATH`, then the
library's include files. `go install
github.com/phonologus/gcon/cmd/gcon@latest` also works: gcon then finds
the library in the module cache. `gcon env` shows where it looks.
Nothing is written beside your sources: there are no `.u1`/`.u2` files.

`gcon build file.icn` writes `file`, an executable that runs the built
program with the installed gcon, as icont's output runs with iconx; it
no longer needs the sources. `gcon dis` lists a program's VM code.

The front end (lexer, parser and name resolution) was forked from
[icontools](https://github.com/phonologus/icontools).

MIT licence; see [LICENSE](LICENSE).
