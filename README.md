# gcon

An implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language (version 9.5) in Go: a translator and a virtual
machine in a single `go build`, with no `make Configure` step.

Status: early, but most of the language runs: `gcon run file.icn` passes
80 of the 89 tests of Icon's own test suite (the rest need the
preprocessor, C external functions, or differ by design in Unicode
csets). See [docs/design.md](docs/design.md) and
[docs/development.md](docs/development.md).

`link` uses Icon source: `link foo` finds `foo.icn` in the current
directory, then in the directories of `IPATH`. To use the Icon Program
Library, point `IPATH` at its source, for example
`IPATH=/path/to/icon/ipl/procs`. Nothing is written beside your
sources: there are no `.u1`/`.u2` files.

The front end (lexer, parser and name resolution) was forked from
[icontools](https://github.com/phonologus/icontools).

MIT licence; see [LICENSE](LICENSE).
