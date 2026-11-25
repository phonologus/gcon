# gcon

An implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language (version 9.5) in Go: a translator and a virtual
machine in a single `go build`, with no `make Configure` step.

Status: front end in place; translator and VM not yet started. See
[docs/design.md](docs/design.md) and [docs/development.md](docs/development.md).

The front end (lexer, parser and name resolution) was forked from
[icontools](https://github.com/phonologus/icontools).

MIT licence; see [LICENSE](LICENSE).
