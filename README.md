# gcon

An implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language (version 9.5) in Go: a translator and a virtual
machine in a single `go build`, with no `make Configure` step.

Status: early. `gcon run file.icn` runs programs using integers,
strings, lists, procedures, generators, co-expressions and the control
structures. See
[docs/design.md](docs/design.md) and [docs/development.md](docs/development.md).

The front end (lexer, parser and name resolution) was forked from
[icontools](https://github.com/phonologus/icontools).

MIT licence; see [LICENSE](LICENSE).
