# Icon (Gcon) for Visual Studio Code

Support for the [Icon programming language](https://www.cs.arizona.edu/icon/)
as [Gcon](https://github.com/phonologus/gcon) reads it:

- syntax highlighting, with Gcon's `\u` escapes, cset ranges, uset
  literals and built-in functions;
- diagnostics as you type: syntax errors, the Icon translator's checks
  (undeclared identifiers, redeclarations, invalid keywords) and
  `gcon vet`'s warnings of likely run-time errors;
- hover documentation for your procedures, records and globals (from
  their comments) and for built-in functions and keywords;
- go to definition, find references, highlight, rename;
- completion of names in scope, built-ins, `&keywords` and record
  fields;
- signature help;
- formatting with `gcon fmt`, on save by default.

The language features come from Gcon's language server, `gcon lsp`. The
extension is a fork of icontools' Icon extension, which runs icontools'
`iconls`; the two both claim `.icn` files, so install one or the other.

## Requirements

Build Gcon and put its `bin` on your `PATH` (see the
[quick start](https://github.com/phonologus/gcon#quick-start)), or set
`gcon.path`. For full documentation of the built-in functions on hover
(optional; the Icon website's documentation cannot be redistributed, so
it is fetched to a local cache):

```sh
gcon lsp fetch-docs
```

## Installing

The extension is not on the Marketplace. Build a package and install it:

```sh
cd editors/vscode
npm install
npm run package                     # writes gcon-0.5.0.vsix
code --install-extension gcon-0.5.0.vsix
```

## Settings

| Setting | Description |
|---|---|
| `gcon.path` | Path to `gcon` (default: found on the `PATH`). |
| `gcon.icon95` | Also warn of Gcon's additions that Icon 9.5 lacks, as `gcon vet -icon95` does, for code that must also run under Arizona Icon. |
| `gcon.ipath` | More directories in which to find the sources of linked files. Gcon already searches the current directory, `$IPATH` and its own Icon Program Library. |
| `gcon.trace.server` | Trace the protocol in the Output panel. |

Format on save, 3-space indentation and spaces are on by default for
Icon files; change them under `"[icon]"` in your settings.

## Unicon

The Unicon extensions also claim `.icn` files (as the `unicon`
language). If both are installed, choose per workspace with, for
example:

```json
"files.associations": { "*.icn": "icon" }
```

## Development

`npm run test:unit` tests the TextMate grammar, which
`scripts/gen-grammar.js` generates with the built-in functions of Gcon's
catalogue (`../../src/builtin.inc`). `npm test` also runs the
integration tests in a downloaded VS Code against `../../bin/gcon`;
`VSCODE_TEST_VERSION` and `VSCODE_TEST_CACHE` pick a version and a copy
already downloaded (`test/runTest.ts`). `../test.sh` runs these with the
grammar's and Zed's tests.
