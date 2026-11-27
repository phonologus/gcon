# Icon (Gcon) for Zed

Icon language support for [Zed](https://zed.dev), as Gcon reads Icon:
syntax highlighting (with Gcon's `\u` escapes, cset ranges, uset literals and
built-ins), outline and bracket matching, from the grammar in
`../tree-sitter-gcon`, and Gcon's language server, `gcon lsp`:
diagnostics and `gcon vet` warnings, hover with documentation, go to
definition, find references, completion, signature help, rename and
formatting with `gcon fmt`.

It is a fork of icontools' Icon extension, which runs icontools'
`iconls`. The two both claim `.icn` files: install one or the other.

## Setup

Build Gcon and put its `bin` on your PATH (see the
[quick start](https://github.com/phonologus/gcon#quick-start)); the
extension runs `gcon lsp`. Then install this extension: in Zed, run
`zed: install dev extension` and choose this directory (`editors/zed`).
Zed builds it, which needs Rust installed through rustup.

For full documentation of the built-in functions on hover (optional;
the Icon website's documentation cannot be redistributed, so it is
fetched to a local cache):

```sh
gcon lsp fetch-docs
```

To format with `gcon fmt` when saving, turn on format-on-save for Icon
in Zed's `settings.json` (Zed's default is off, and an extension cannot
change that). `editor: format` works without it. Files with syntax
errors are not formatted.

```json
"languages": {
  "Icon": { "format_on_save": "on" }
}
```

Optional settings:

```json
"lsp": {
  "gcon": {
    "binary": {
      "path": "/path/to/gcon/bin/gcon",
      "arguments": ["lsp"]
    },
    "initialization_options": {
      "icon95": true,
      "ipath": ["/path/to/more/libraries"]
    }
  }
}
```

- `binary.path`: the `gcon` to run, if it is not on your PATH. Give
  `binary.arguments` as `["lsp"]` with it: Zed runs a binary named in
  the settings itself, with the settings' arguments, without asking the
  extension, so without them it runs plain `gcon`, which prints its
  usage, and the server fails to start. Or leave `binary` out and put
  Gcon's `bin` on your PATH: then the extension supplies `lsp`.
- `icon95`: also warn of Gcon's additions that Icon 9.5 lacks, as
  `gcon vet -icon95` does, for code that must also run under Arizona
  Icon.
- `ipath`: more directories in which to find linked files' sources.
  Gcon already searches the current directory, `IPATH` and its own Icon
  Program Library.

## Development

`extension.toml` pins the grammar at a commit of this repository
(`editors/test.sh` checks that it is the current grammar). Zed fetches
the pinned commit from GitHub; to try a grammar that is not pushed yet,
point `repository` at the checkout, as `file:///path/to/gcon`, for the
time being.
