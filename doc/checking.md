# How Gcon is checked

Icon's own C sources are the authority on the language, so Gcon is
checked against Arizona's icont and iconx:

- **The language:** 88 of the 89 programs in Icon's test suite give
  iconx's output exactly (`vm/tests/conformance.sh`); the other,
  `ilib`, differs only in a line that depends on the clock.
- **The front end** (preprocessor, parser and name resolution) is
  compared with icont itself on every Icon file in Arizona's source tree
  (`src/tests/oracle.sh`).
- **Graphics** are compared pixel for pixel with Arizona Icon's X11
  images (`ref.sh`), and the graphics programs Gcon ships are run under
  Gcon and under Arizona's own iconx, built with X11, their output
  compared (`arizona.sh`). They are also run in real windows, with
  scripted keys and clicks (`window.sh`).
- The runtime is tested under the address and undefined-behaviour
  sanitizers.

For Gcon's own additions (`fmt`, `lsp`, `vet`, usets and `uclass`),
Gcon's recorded results are the reference: golden files, test programs,
and runs of 318 programs from the test suite and the IPL
(`vm/tests/expected.sh`). The test drivers are described in `design/development.md`.

## History

Gcon was written first in Go. It was then ported to C part by part, and
each part was proved to give the same results as the Go before the Go
was removed. The Go is kept as the tag `go-reference`. The `g` came from
Go; it can stand for graphics now. The front end began as a fork of
[icontools](https://github.com/phonologus/icontools), which remains a
standalone formatter and language server for Arizona Icon.

## Licence

Gcon is under the MIT licence; see the root's `LICENSE`. The Icon Program
Library in `lib/ipl` is in the public domain; `NOTICE`
reproduces the copyright notices on the card bitmaps in
`lib/ipl/gprocs/cardbits.icn`, as a precaution. The display servers'
vendored fonts and stb_truetype (`deps/fixed`, `deps/stb`) are in the
public domain, and the Unicode Character Database in `deps/unicode` is under the Unicode
License v3 (`deps/unicode/LICENSE`).
