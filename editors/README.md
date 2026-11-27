# Editor support

Editor support for Icon as Gcon reads it, around Gcon's language server,
`gcon lsp`:

| Path | Contents |
|---|---|
| `tree-sitter-gcon/` | the tree-sitter grammar: Icon 9.5 with Gcon's escapes, cset ranges and uset literals ([README](tree-sitter-gcon/README.md)) |
| `zed/` | the Zed extension: the grammar's highlighting and outline, and `gcon lsp` ([README](zed/README.md)) |
| `vscode/` | the VS Code extension: a TextMate grammar, and `gcon lsp` ([README](vscode/README.md)) |
| `gen.sh` | writes Gcon's built-in function names, from `../src/builtin.inc`, into the highlight queries |
| `test.sh` | tests all three |

All three are forks: of [tree-sitter-icon](https://github.com/phonologus/tree-sitter-icon)
and of [icontools](https://github.com/phonologus/icontools)' extensions,
which serve Arizona Icon with icontools' `iconls`. Install Gcon's or
icontools' extension, not both: they both claim `.icn` files.

None of this is needed to build or use Gcon, and `../build.sh` does not
build it: the extensions are written in their editors' languages (Rust
for Zed, TypeScript for VS Code), and building them needs those
toolchains. The grammar's generated parser is committed, so building it
needs only a C compiler (editors do that themselves).

Any other editor with a language-server client can run `gcon lsp`
(over standard input and output); its initialization options are
`icon95` (a boolean, as `gcon vet -icon95`) and `ipath` (a list of more
directories for `link`).
