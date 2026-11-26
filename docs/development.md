# Development

```sh
./build.sh
```

builds gcon into the repository, which is a gcon root (`lib/ipl`), so
that gcon finds the library (`gcon env`): `bin/gcon`, and gcon-vm and
the display server into `libexec`. The C is C11 with the GCC and Clang
extensions already used (computed goto), `-Wall -Wextra` clean, and
uses libc only; test under the address and undefined-behaviour
sanitizers. Scripts are sh, sed and awk.
`tools/ipl/update.sh` refreshes `lib/ipl` from the reference clone.

## Layout

| Path | Purpose |
|---|---|
| `src/` | the `gcon` command: lexer, parser, preprocessor, name resolution, compiler, driver, `gcon fmt`, `gcon lsp` ([src/README.md](../src/README.md)) |
| `vm/` | gcon-vm, the runtime ([vm/README.md](../vm/README.md)) |
| `graphics/` | gcon-graphics, the display server |
| `lib/ipl` | the Icon Program Library's source (public domain), searched after `IPATH`/`LPATH` |
| `tools/icontok` | C driver exposing icont's token stream and parse trees |
| `tools/iconx` | builds the reference icont and iconx; generates expected test output |
| `tools/ipl` | copies the IPL into `lib/ipl`, recording `lib/ipl/UPSTREAM` |
| `experiments/` | measurements behind design decisions; not part of gcon |

gcon was written in Go and ported to C part by part, each part proved
identical to the Go; the Go is kept as the tag `go-reference`. Its front
end was forked from icontools at commit 9cf9615. Syntax facts are in
[syntax.md](syntax.md), and the design in [design.md](design.md).

## The two oracles

Icon's C sources, icont and iconx, are the authority for everything
Icon defines; gcon itself (its recorded runs and probes) for what gcon
adds (design.md, "Two oracles").

Many tests use the Icon sources (<https://github.com/gtownsend/icon>) as
ground truth and as a corpus of real Icon code (its `tests` and the Icon
Program Library). Clone it next to this repository, as `../icon` (or set
`$ICON_SRC` for `vm/tests/conformance.sh`).

## The front end against icont

`tools/icontok` is a small C driver linked with icont's front end: it
prints icont's token stream, or (`-t`) the parse tree icont builds for
each procedure. Its `build.sh` builds it, and icont, in a scratch copy
of the Icon sources (default `$TMPDIR/icontok-build`), so the clone is
never modified. `src/tests/oracle.sh` compares gcon's tokens, trees
(also on randomly damaged copies), preprocessor output (`icont -E`),
semantic diagnostics (`icont -u`) and translation errors (`icont -c`)
with icont's over the corpus.

`src/tests/fmt.sh` and `src/tests/lsp.sh` check `gcon fmt` and `gcon
lsp` against themselves: golden files recorded from gcon
(`src/tests/fmt/`, `src/tests/lsp/`; rewrite them with `-update`, and
read the difference before committing), and, over the whole corpus,
properties that need no oracle (no crash; formatting keeps the tokens
and is idempotent; every request answered). `src/tests/lit/` holds
probes of gcon's literals, whose diagnostics must be the errors `gcon
build` reports.

## The runtime against iconx

Expected output comes from the reference implementation, never from
gcon or from hand. `vm/tests/programs/*.icn` are gcon's own test
programs; for each, `FILE.std` is iconx's standard output and
`FILE.exit` its exit status if nonzero (`FILE.dat` is standard input
and `FILE.args` the arguments, if present). After adding or changing a
program, regenerate its expectation:

```sh
tools/iconx/expect.sh vm/tests/programs/NAME.icn
```

`tools/iconx/build.sh` builds icont and iconx in a scratch copy of the
reference tree (default `$TMPDIR/iconx-build`); `expect.sh` calls it.

A known difference in the oracle itself: iconx formats reals with C's
`printf("%.10g")`, and macOS's C library keeps trailing zeros when a
value is an exact tie in exponent form (`1e10+5` prints as
`1.000000000e+10`, where the C standard and glibc give `1e+10`). gcon
follows the standard, so test programs avoid such values.

`vm/tests/conformance.sh` runs Icon's own suite (`icon/tests/general`)
as its `Test-icon` script does, in a copy of that directory, and
compares each program's output with iconx's (`FILE.std`).
`vm/tests/conformance.txt` lists the tests gcon passes; the test fails
if a listed test fails or an unlisted one passes. After making more
tests pass, `vm/tests/conformance.sh -update`.

`vm/tests/expected.sh` checks the IPL's programs and Icon's suite
against recorded runs (`vm/tests/expected/`); the other runtime tests
are described in [vm/README.md](../vm/README.md).
