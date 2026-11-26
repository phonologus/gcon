# The C front end

gcon's translator and tools in C, ported part by part from the Go
reference (tag `go-reference`, in `../internal`), each part proved
identical before the next (`../docs/design.md`, "gcon in C").

| File | Ported from | Status |
|---|---|---|
| `util.c` | (Go's standard library: buffers, UTF-8, big integers) | |
| `token.c`, `tokens.inc` | `internal/token` | done |
| `lexer.c` | `internal/lexer` | done |
| `lit.c` | `internal/lit` | done |
| `syntax.c` | `internal/syntax` | done |
| `ipp.c` | icont's `src/common/ipp.c` (output as `internal/ipp`) | done |
| `builtin.c`, `builtin.inc` | `internal/builtin` | done |
| `sema.c` | `internal/sema` (diagnostics icont's `tsym.c`, `lglob.c`, `lsym.c`) | done |

`tokens.inc` is generated from the Go's token table by
`tests/gentoken.sh`, and `builtin.inc` from its catalogue of built-ins
by `tests/genbuiltin.sh` (while the Go is the reference).

## Testing

    tests/diff.sh [lex|parse|walk|pp|sema]

builds `tests/dump.c` with the address and undefined-behaviour
sanitizers, and the Go reference's `tools/refdump`, and checks that
both write the same canonical form for every file of the corpus: the Icon tree beside the checkout,
gcon's library, test data and experiments, `../icn`, the edge cases in
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

The parser does not recurse (`../docs/design.md`, "Two oracles"):
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
(`../docs/design.md`, "Two oracles"), with no Go involved:
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
output given gcon's form (a line for each line read, and a map of where
each came from) and its nested `$if` skip made iterative. `oracle.sh`
checks it against `icont -E`, with the symbols that icont defines: the
same lines that are not blank, each from the same file and line (from
icont's `#line` markers), and the same errors, for the Icon tree and
the edge cases in `tests/pp` (every directive error, includes nested,
circular, and by icont's rule of the name's last component on the
path). At its port: 1,208 files identical, 181,001 lines with their
positions, 101 errors. `diff.sh pp` checks it against the Go (2,795
files identical); the port found three places where the Go was not
icont's, fixed in both (`../docs/design.md`). Deep nesting (a million
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
compared there. `diff.sh sema` checks it against the Go (each file
resolved preprocessed and raw: 2,809 identical), and whole directories
as one program (the IPL's procs, gprocs and progs, the reference tests,
`../icn`) are identical too. The port found the Go's wording of five
diagnostics not icont's, fixed in both (`../docs/design.md`).
