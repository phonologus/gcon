# Development

```sh
go test ./...
go vet ./...
gofmt -l .
```

## Layout

| Path | Purpose |
|---|---|
| `cmd/gcon` | the `gcon` command: `run`, `build`, `dis`, `version` |
| `internal/run` | translate, build and run a program; end-to-end, round-trip and conformance tests |
| `internal/compile` | syntax tree to VM code (Proebsting's translation) |
| `internal/vm` | values, instructions, the interpreter loop, operators and built-ins |
| `internal/lit` | decoding of string, cset and numeric literals |
| `internal/token` | token kinds and semicolon-insertion flags (from icont's `lextab.h`) |
| `internal/lexer` | lossless lexer: trivia, positions, inserted semicolons |
| `internal/syntax` | error-tolerant parser producing a lossless syntax tree (icont's `grammar.h`) |
| `internal/sema` | name resolution and semantic diagnostics |
| `internal/builtin` | built-in catalogue (generated) and reference-documentation cache |
| `internal/corpus` | locates the reference Icon sources used in tests |
| `internal/oracle` | runs icont's own front end for tests (via `tools/icontok`) |
| `tools/icontok` | C driver exposing icont's token stream and parse trees |
| `tools/mkbuiltin` | generator for `internal/builtin/catalog_gen.go` |
| `tools/iconx` | builds the reference icont and iconx; generates expected test output |
| `experiments/` | measurements behind design decisions; not part of gcon |

The front end (`token` through `oracle`, and both tools) was forked from
icontools at commit 9cf9615. The two now evolve separately. Syntax facts
are in [syntax.md](syntax.md), and the translator and VM design is in
[design.md](design.md).

## The reference Icon sources

Many tests use the Icon sources (<https://github.com/gtownsend/icon>) as
ground truth (token tables, grammar, symbol-table rules) and as a corpus of
real Icon code (its `tests` and the Icon Program Library). Clone it next to
this repository, as `../icon`, or set `$ICON_SRC` to its location. Tests
that need it are skipped if it is absent.

## Testing against icont

`tools/icontok` is a small C driver linked with icont's front end: it
prints icont's token stream, or (`-t`) the parse tree icont builds for
each procedure. `build.sh` builds it, and icont, in a scratch copy of the
Icon sources (default `$TMPDIR/icontok-build`), so the clone is never
modified:

```sh
ICONTOK=$(tools/icontok/build.sh) go test ./...
```

With `$ICONTOK` set:

- the lexer's `TestOracle` compares token streams on every corpus file;
- the parser's `TestOracle` compares the parse tree of every procedure, and
  `TestMutations` compares acceptance and trees on randomly damaged copies
  of corpus files (`MUTATIONS=30000` for a thorough run);
- `sema`'s `TestOracle` compares semantic diagnostics with `icont -u`.

Files that need the preprocessor are compared on their `icont -E`
expansion, and `ipp`'s `TestOracle` compares gcon's preprocessor with
`icont -E` on every corpus file.

## The built-in catalogue

`internal/builtin/catalog_gen.go` is generated from the Icon sources
(function and keyword tables, the runtime's documentation strings, and the
preprocessor's predefined symbols):

```sh
go generate ./internal/builtin
```

## Testing the VM against iconx

Expected output always comes from the reference implementation, never
from gcon or from hand. `internal/run/testdata/*.icn` are gcon's own
test programs; for each, `FILE.std` is iconx's standard output and
`FILE.exit` its exit status if nonzero (`FILE.dat` is standard input
and `FILE.args` the arguments, if present). After adding or changing a
program, regenerate its expectation:

```sh
tools/iconx/expect.sh internal/run/testdata/NAME.icn
```

`tools/iconx/build.sh` builds icont and iconx in a scratch copy of the
reference tree (default `$TMPDIR/iconx-build`); `expect.sh` calls it.

A known difference in the oracle itself: iconx formats reals with C's
`printf("%.10g")`, and macOS's C library keeps trailing zeros when a
value is an exact tie in exponent form (`1e10+5` prints as
`1.000000000e+10`, where the C standard, glibc and Go give `1e+10`).
gcon follows the standard, so test programs avoid such values.

Each program runs in a temporary directory, so it may create files, with
standard input from `FILE.dat`.

`TestConformance` runs the reference suite (`icon/tests/general`) as its
`Test-icon` script does, in a temporary copy of that directory, so the
reference tree is never written to. `internal/run/testdata/conformance.txt` lists the
tests gcon passes; the test fails if a listed test fails or an unlisted
one passes. After making more tests pass:

```sh
go test ./internal/run -run Conformance -args -update
```
