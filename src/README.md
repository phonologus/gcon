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

    tests/diff.sh [lex|parse]

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

Known difference: the parser recurses (about a dozen C frames per
level of nesting), so input nested thousands deep, such as 20,000
parentheses, overflows the C stack where the Go, and icont (whose yacc
stack grows), do not. To be settled before the driver is ported.
