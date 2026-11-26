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

`tokens.inc` is generated from the Go's token table by
`tests/gentoken.py` (while the Go is the reference).

## Testing

    tests/diff.sh

builds `tests/dump.c` with the address and undefined-behaviour
sanitizers, and the Go reference's `tools/refdump`, and checks that
both write the same canonical form (tokens, trivia, decoded literals,
errors) for every file of the corpus: the Icon tree beside the checkout,
gcon's library, test data and experiments, `../icn`, the edge cases in
`tests/lex.icn` and `tests/lex2.icn`, and variants made with a fixed
seed (truncations, changed bytes, random bytes). At the lexer's port:
2,780 files identical (and 3,000 more mutated ones, once).
