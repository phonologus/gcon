# Editors

Gcon's language server, `gcon lsp`, gives diagnostics as you type
(including `gcon vet`'s warnings), hover documentation, go to definition,
references, completion, signature help, rename and formatting. The
extensions for Zed and VS Code are in the root's `editors` directory.
Each one runs the `gcon` on your `PATH`, so [build Gcon](start.md)
first.

For full documentation of the built-in functions on hover, run this
once; the Icon website's documentation cannot be redistributed, so it is
fetched to a local cache:

```sh
gcon lsp fetch-docs
```

The extensions are forks of icontools' Icon extensions, which run
icontools' `iconls`. Both claim `.icn` files, so install one or the
other.

## Zed

You need Rust, installed through [rustup](https://rustup.rs). Zed builds
the extension and the grammar itself.

1. In Zed, open the command palette and run `zed: install dev extension`.
2. Choose the `editors/zed` directory of your Gcon checkout.
3. Optionally, turn on formatting on save in Zed's `settings.json`:

   ```json
   "languages": {
     "Icon": { "format_on_save": "on" }
   }
   ```

Settings such as `icon95` (warn of Gcon's additions, for code that must
also run under Arizona Icon) are in `editors/zed/README.md`.

## VS Code

You need Node.js. The extension is not on the Marketplace, so you build
a package and install it:

```sh
cd editors/vscode
npm install
npm run package                     # writes gcon-0.4.1.vsix
code --install-extension gcon-0.4.1.vsix
```

Formatting on save is on by default for Icon files. The settings
`gcon.path`, `gcon.icon95` and `gcon.ipath` are described in
`editors/vscode/README.md`.

## Other editors

Any editor with a language-server client can run `gcon lsp`, which talks
over standard input and output; [gcon help lsp](help/lsp.md) has its
flags and options. The tree-sitter grammar is in
`editors/tree-sitter-gcon`.
