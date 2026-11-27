# The C front end

Gcon's translator and tools in C, built as `../bin/gcon`. They were
ported part by part from the Go reference (the tag `go-reference`),
each part proved identical before the next (`../design/design.md`, "Gcon
in C"), and the Go then removed, all at once; this file records how
each part was proved. Going forward the oracles are icont and iconx
for what Icon defines, and Gcon's own recorded results for what it adds.

| File | Ported from (in `go-reference`) |
|---|---|
| `util.c` | (Go's standard library: buffers, UTF-8, big integers) |
| `token.c`, `tokens.inc` | `internal/token` |
| `lexer.c` | `internal/lexer` |
| `lit.c` | `internal/lit` |
| `syntax.c` | `internal/syntax` |
| `ipp.c` | icont's `src/common/ipp.c` (output as `internal/ipp`) |
| `builtin.c`, `builtin.inc` | `internal/builtin` |
| `sema.c` | `internal/sema` (diagnostics icont's `tsym.c`, `lglob.c`, `lsym.c`) |
| `prog.c` | `internal/vm`: the program, its constants (csets as `cset.go`), `encode.go` |
| `compile.c` | `internal/compile`, iterative |
| `driver.c`, `main.c` | `internal/run`, `internal/root`, `cmd/gcon` |
| `format.c` | icontools' `internal/format` and `cmd/icfmt` (`gcon fmt`) |
| `json.c` | (Go's `encoding/json`: reading, and writing as it writes) |
| `lsp.c`, `refdocs.c` | icontools' `internal/lsp`, `internal/builtin/docs.go`, `cmd/iconls` (`gcon lsp`) |
| `vet.c` | (new in C: `gcon vet`) |

`tokens.inc` and `builtin.inc` were generated from the Go's token table
and catalogue of built-ins, and are maintained by hand.

## Testing

The tests now are `tests/oracle.sh` (against icont), `tests/fmt.sh`
and `tests/lsp.sh` (Gcon against itself: golden files and properties
over the corpus), and gcon-vm's (`../vm/tests`), which run what Gcon
builds. `tests/dump.c` prints the
front end's view of a file (`dump lex|parse|walk|pp|sema|build FILE`),
for debugging.

Until the Go went, `tests/diff.sh` (retired with it) built
`tests/dump.c` with the address and undefined-behaviour
sanitizers, and the Go reference's `tools/refdump`, and checked that
both wrote the same canonical form for every file of the corpus: the Icon tree beside the checkout,
Gcon's library, test data and experiments, `../icn`, the edge cases in
`tests/lex.icn` and `tests/lex2.icn`, and variants made with a fixed
seed (truncations, changed bytes, random bytes). At the lexer's port:
2,780 files identical (and 3,000 more mutated ones, once).

The forms: `lex`, the tokens, trivia, decoded literals and lexical
errors; `parse`, the syntax tree field by field (tokens as indexes,
absent fields as `-`), the order of the token walk (`syn_children`),
and the errors. `tests/parse.icn` holds error paths the corpus rarely
reaches. At the parser's port: 2,784 files identical (and 1,626 more
mutants with byte runs deleted or duplicated, once, reaching some 50
distinct error messages).

The parser does not recurse (`../design/design.md`, "Two oracles"):
its expression machine keeps its stack on the heap, as icont's parser
does, so nesting has no ceiling but memory, where the Go's goroutine
stack fails at 1 GB (about a million nested parentheses). The `walk`
form (the token walk and errors, no tree: nothing in either dump
recursing) checks deep input: 23 shapes of nesting (brackets, prefix
and right-associative chains, control structures, case, sections,
mixtures, unclosed) 100,000 deep are identical to the Go's, the full
trees 3,000 deep, and the C alone parses them 2,000,000 deep.

    tests/oracle.sh [MUTANTS]

checks the C against icont itself, the authority on Icon's syntax
(`../design/design.md`, "Two oracles"), with no Go involved:
`tests/oracle.c` writes the C front end's view in the forms of
`../tools/icontok` (icont's own lexer and parser, linked from a scratch
build of the Icon tree), the token stream and a tree per procedure as
icont's grammar actions build it, and the two must agree on every file
of the Icon tree (on its `icont -E` expansion where it needs the
preprocessor), then on mutants (a token deleted, duplicated or swapped).
At its port: tokens identical in 1,188 files (162 preprocessed), trees
in 1,177 (7,750 procedures), 10 rejected by both, 1 with only
non-syntax errors (skipped), 15 not preprocessable by icont; 300
mutants, no disagreement. A planted bug (`^` made left-associative) is
caught, in `tests/general/parse.icn`.

The preprocessor is icont's own code (`ipp.c`, public domain), its
output given Gcon's form (a line for each line read, and a map of where
each came from) and its nested `$if` skip made iterative. `oracle.sh`
checks it against `icont -E`, with the symbols that icont defines: the
same lines that are not blank, each from the same file and line (from
icont's `#line` markers), and the same errors, for the Icon tree and
the edge cases in `tests/pp` (every directive error, includes nested,
circular, and by icont's rule of the name's last component on the
path). At its port: 1,208 files identical, 181,001 lines with their
positions, 101 errors. `diff.sh pp` checked it against the Go (2,795
files identical); the port found three places where the Go was not
icont's, fixed in both (`../design/design.md`). Deep nesting (a million
`$if`s, or a chain of a million definitions) has no ceiling; the chain
is slow, as icont's 256-bin definition table makes it.

Name resolution (`sema.c`) is checked against icont's own diagnostics
by `oracle.sh`: for each file of the Icon tree that stands alone and
translates, and the programs in `tests/sema` (single files, and each
directory linked as one program), the C's diagnostics must be `icont
-u`'s, text, file and line: its translator's redeclarations and
invalid keywords, and its linker's redeclarations across files and
undeclared identifiers in the procedures it keeps (those reachable from
main and invocable names; the file named as icont's linker names it,
by where the procedure's code came from). At its port: 568 programs
identical, 438 diagnostics; files that link others (642) are not
compared there. `diff.sh sema` checked it against the Go (each file
resolved preprocessed and raw: 2,809 identical), and whole directories
as one program (the IPL's procs, gprocs and progs, the reference tests,
`../icn`) are identical too. The port found the Go's wording of five
diagnostics not icont's, fixed in both (`../design/design.md`).

The compiler (`compile.c`) keeps its own stack of frames, one kind for
each function of the Go's recursive compiler, each making its side
effects (labels, temporaries, constants, globals, instructions) in the
Go's order, so that its output is the Go's byte for byte; the format's
definitions are shared with gcon-vm's loader (`../vm/src/format.h`), and
its tables of operators and functions are gcon-vm's
(`../vm/src/gen_tables.c`). `diff.sh build` compiled each file as a
program (`dump build`, `refdump build`: the encoding, with the build
information test=1, or the errors) and compared them byte for byte: at
its port 2,811 identical, 1,714 of them compiled programs (6.8 MB). Also
identical: the 310 programs of the IPL's progs and gprocs with the files
they link (4,207 files); and 19 shapes of nesting 20,000 deep, which the
C also compiles a million deep (where the Go's stack gives out) in
seconds. `oracle.sh` checks the compiler's errors against icont's
(`tests/compile/err-*.icn`). The port found two bugs in the Go, fixed:
compiling deep nesting took time quadratic in the depth, and break's
expression could overwrite the enclosing loop or scan
(`../vm/tests/programs/break*.icn`).

The driver (`driver.c`, `main.c`) was
checked end to end against the Go's `gcon`: `gcon env` in several
environments; `gcon build` and `gcon dis` of the 618 files of the
reference tests, the IPL's progs and gprogs, Gcon's test data and
`../icn` (604 built files identical, 36.6 MB, but for the header's
`GCBIN=` line naming each `gcon`; messages and listings identical); `gcon
run` of source and of built programs, with arguments, and each error
path (missing files, unresolved links with and without a root, syntax
errors, flags); and gcon-vm's tests run with it as `$GCON`
(`../vm/tests/expected.sh`, `programs.sh`). Under the
sanitizers, it builds the 476 programs of the IPL and `../icn` cleanly.

The formatter (`format.c`, `gcon fmt`) is icontools' (`icfmt`),
whose tree is Gcon's (Gcon's front end is icontools' fork), on Gcon's
front end, iterative like the rest. While it was ported, `tests/fmt.sh`
checked it against icfmt (built from `../icontools`): the same output,
messages and exit status for every file of the corpus and its damaged
variants (2,822, 1,838 of them changed by formatting), the token
sequence of every formatted file kept (inserted semicolons included),
and the flags (`-l`, `-w`, `-d`, standard input, errors). Gcon's
literals (`\u`, `\U`, cset ranges) are tokens like any other, kept
byte for byte. Now Gcon is its own oracle (`../design/design.md`):
`tests/fmt.sh` checks golden output for the repository's own sources
and the flags (`tests/fmt/`), and, over the corpus and its variants,
that formatting never crashes, keeps every token and is idempotent.

The language server (`lsp.c`, `gcon lsp`) is icontools' `iconls` on
Gcon's front end, finding linked and included files through Gcon's
root, its diagnostics Gcon's (icont's wording, the other declaration
as related information). `json.c` writes JSON as Go does, so replies
compared byte for byte. While it was ported, `tests/lsp.sh` ran
sessions made by `tests/lspsession.c` on both servers (iconls built
from `../icontools`), in parallel: basic (symbols, formatting,
diagnostics) for every file of the corpus, 1,933 sessions identical;
features (hover, definition, references, highlights, completion,
signature help, rename, workspace symbols at every seventh
identifier) for every tenth file, 170 sessions and 32,069 messages
identical, with or without the reference pages. Now it checks Gcon
against itself: golden replies for probes in the repository
(`tests/lsp/`, hovers on reference-page fixtures in `tests/lsp/ref`),
and a reply to every request in sessions over the whole corpus
(`FULL=1`: every file and variant; `SANITIZE=1`: the sanitizers).

Gcon's additions to the language are checked against Gcon itself:
`tests/lsp.sh` also checks that the language server's diagnostics for
the probes in `tests/lit` (every literal error, and good literals) are
the errors `gcon build` reports.

`gcon vet` (`vet.c`; `../design/design.md`, "gcon vet") is checked by
`tests/vet.sh`: golden findings for the probes in `tests/vet` and for
the programs of the Icon tree and `../icn` it was tuned on, the
language server reporting the probes' findings, and no crash over the
corpus.
