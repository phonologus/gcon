# tree-sitter-gcon

A [tree-sitter](https://tree-sitter.github.io/) grammar for Icon as gcon
reads it: Icon 9.5, with gcon's lexical additions: the escapes
`\uXXXX` and `\UXXXXXX`, ranges in cset literals (`'a..z'`), and uset
literals (`u'...'`, a `uset` node with a cset's parts). The
editors' extensions in `../zed` use it.

It is a fork of [tree-sitter-icon](https://github.com/phonologus/tree-sitter-icon)
(at 6070fd3), which follows the yacc grammar of the Icon translator
(`src/h/grammar.h` in the Icon sources): the precedence levels of `expr`
… `expr11`, control structures that extend as far to the right as
possible, and, through an external scanner (`src/scanner.c`), Icon's
semicolon insertion. The one change is in literals: where
tree-sitter-icon makes a string or cset a single token, here escapes are
`escape_sequence` nodes and a cset's `..` is a `range_operator`, so that
editors can show them. The trees are otherwise the same.

Highlighting queries are in `queries/` (Neovim capture conventions); the
built-in function names in them come from gcon's catalogue
(`../gen.sh`).

## Verification

`../test.sh` runs the tests in `test/corpus`, checks that `src/` is
generated from `grammar.js`, and parses every Icon file in Arizona
Icon's tree (`../../../icon`) and in gcon's own tests and library: the
files that do not parse are exactly those recorded in `failing.txt`
(preprocessor tests whose `$ifdef` branches do not parse as a whole,
deliberate errors, and include files that are not programs), the same
files tree-sitter-icon fails on.

## Development

```sh
npm install
npx tree-sitter generate
npx tree-sitter test
```

`src/parser.c` and the rest of `src/` are generated, and committed, so
that editors can build the grammar without the tree-sitter CLI.

## Licence

MIT; see `LICENSE`.
