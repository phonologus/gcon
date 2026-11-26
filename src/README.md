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

`tokens.inc` is generated from the Go's token table by
`tests/gentoken.py` (while the Go is the reference).

## Testing

    tests/diff.sh [lex|parse|walk]

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
