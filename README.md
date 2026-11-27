# gcon

gcon is an implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language, version 9.5, written in C. It has a translator,
a virtual machine and a graphics display server. One script builds the
lot, and there is no `make Configure` step. Compared with Arizona's
icont/iconx, it:

- **Runs Icon 9.5 programs unchanged.** They pass Icon's own test suite,
  and the Icon Program Library runs, graphics included.
- **Runs about twice as fast** as iconx, and co-expressions run hundreds
  of times faster ([Performance](#performance)).
- **Uses a modern graphics backend:** SDL3 and Blend2D, in place of X11,
  with antialiasing, HiDPI and native windows.
- **Keeps Icon 9.5's byte strings,** with UTF-8 help where asked for
  ([Differences from Icon 9.5](#differences-from-icon-95)).
- **Comes with modern tools:** one `gcon` command that runs, builds,
  formats and checks programs and serves as a language server.

```
$ cat hello.icn
procedure main()
   write("h", uchar(233), "llo, wörld: ", *"wörld", " bytes")
end
$ gcon run hello.icn
héllo, wörld: 6 bytes
```

## Quick start

You need a C compiler. The display server also needs a C++ compiler and
CMake; without them, everything except windows works.

```sh
git clone https://github.com/phonologus/gcon
cd gcon
./build.sh                          # builds into the checkout; no network
export PATH=$PWD/bin:$PATH          # the only set-up needed
gcon run hello.icn
```

- On **macOS**, install the Xcode Command Line Tools and CMake.
- On **Linux**, install the packages listed in
  `graphics/linux-packages.txt` (Debian, Ubuntu) or
  `graphics/linux-packages-fedora.txt` (Fedora).

The checkout becomes a *gcon root*, in the way a Go checkout is a
GOROOT. `gcon` finds the root from its own location, so you don't need
`GCONROOT` or any other variable. The Icon Program Library is part of
the root: `link graphics` just works. `IPATH` and `LPATH` still add
directories to search, as with icont, and `gcon env` shows where gcon
looks.

```sh
gcon run prog.icn [args]            # translate and run; nothing is written
gcon build prog.icn                 # write ./prog, a runnable program
gcon fmt -w *.icn                   # format sources in place
gcon vet prog.icn                   # report likely mistakes
gcon lsp                            # language server, for your editor
```

No `.u1` or `.u2` files are written anywhere: programs are linked from
source each time.

## Editor set-up

gcon's language server, `gcon lsp`, gives diagnostics as you type
(including `gcon vet`'s warnings), hover documentation, go to definition,
references, completion, signature help, rename and formatting. The
extensions for Zed and VS Code are in `editors/`. Each one runs the
`gcon` on your `PATH`, so finish the quick start first. For full
documentation of the built-in functions on hover, run this once; the Icon
website's documentation cannot be redistributed, so it is fetched to a
local cache:

```sh
gcon lsp fetch-docs
```

The extensions are forks of icontools' Icon extensions, which run
icontools' `iconls`. Both claim `.icn` files, so install one or the
other.

### Zed

You need Rust, installed through [rustup](https://rustup.rs). Zed builds
the extension and the grammar itself.

1. In Zed, open the command palette and run `zed: install dev extension`.
2. Choose the `editors/zed` directory of your gcon checkout.
3. Optionally, turn on formatting on save in Zed's `settings.json`:

   ```json
   "languages": {
     "Icon": { "format_on_save": "on" }
   }
   ```

Settings such as `icon95` (warn of gcon's additions, for code that must
also run under Arizona Icon) are in
[editors/zed/README.md](editors/zed/README.md).

### VS Code

You need Node.js. The extension is not on the Marketplace, so you build
a package and install it:

```sh
cd editors/vscode
npm install
npm run package                     # writes gcon-0.3.0.vsix
code --install-extension gcon-0.3.0.vsix
```

Formatting on save is on by default for Icon files. The settings
`gcon.path`, `gcon.icon95` and `gcon.ipath` are described in
[editors/vscode/README.md](editors/vscode/README.md).

### Other editors

Any editor with a language-server client can run `gcon lsp`, which talks
over standard input and output. The tree-sitter grammar is in
`editors/tree-sitter-gcon`.

## Differences from Icon 9.5

Icon 9.5 programs behave as they do under iconx. The differences below
are deliberate additions. `gcon vet -icon95` reports any use of them,
for code that must also run under Arizona Icon.

### Strings and Unicode

Strings are bytes, and csets are sets of the 256 byte values, exactly as
in Icon 9.5 (and as strings are in Go). Every string operation counts
and reads bytes, so binary data and Latin-1 text behave as under iconx.
Positions are always byte positions.

- **Literals** gain Go's `\u` and `\U` escapes, which give a code
  point's UTF-8 bytes: `"caf\u00e9"` is the same 5 bytes as `"café"` in
  a UTF-8 source file.
- **Csets** gain ranges: `'a..zA..Z_'`, `'\x00..\x1f'`. Both ends of a
  range are single bytes.
- **`image`** shows a valid, printable UTF-8 sequence as itself, so
  `image("café")` is `"café"`; iconx writes `"caf\xc3\xa9"`. Both are
  literals of the same bytes. Other bytes from `\x80` are escaped as in
  9.5.
- **Usets** are gcon's sets of characters for UTF-8 text, beside the
  cset: their members are the ASCII characters, the 128 bytes from
  `\x80` and every Unicode code point. A uset literal is written
  `u'...'` (`u'α..ω'`, `u'é'`), `uset(x)` converts, and `&uset` is
  every character. Given a uset, `any`, `many`, `upto`, `bal` and
  `trim` step over whole UTF-8 characters, so `tab(any(&uset))` is the
  next character of the subject; given a cset, they step over bytes, as
  in 9.5. Positions are always byte positions.
- **Csets and usets never mix:** `c ++ u`, `cset(u)` or a cset and a
  uset in one `bal` is run-time error 128, and `gcon vet` reports such
  mixes. `uset(c)` is the one conversion between them.
- **New built-ins:** `uset(x)`, `uord(s)` (the code point of one UTF-8
  character), `uchar(i)` (the UTF-8 encoding of a code point) and
  `uvalid(s)`.
- **Unicode character classes** are a library module, not language:
  `link uclass`, and `uclass("L")`, `uclass("Nd")` or `uclass("Greek")`
  is that general category or script as a uset (Unicode 17.0.0).

The details are in [docs/design.md](docs/design.md), "Strings and
Unicode".

### Graphics

The display server is [icon-sdl](https://github.com/phonologus/icon-sdl)'s
SDL3 + Blend2D backend, run as a separate process
(`libexec/gcon-graphics`). It is not an X11 client.

- **The preprocessor defines `_SDL_GRAPHICS`, not `_X_WINDOW_SYSTEM`**,
  and `&features` says `SDL graphics`, so programs that test for X take
  their portable path. Keys are still reported as X keysyms: gcon's
  copy of the library carries icon-sdl's two small changes, made by
  `tools/ipl/patch.sh`, so that `keysyms.icn`'s key names work.
- **Drawing is antialiased and HiDPI.** One Icon pixel is one point,
  drawn at the display's scale. Axis-aligned lines, fills, patterns,
  images and all 16 drawops match Arizona Icon's X11 output exactly; the
  edges of circles, slanted lines and thin arcs differ by a few pixels
  (the tolerances are recorded in `vm/tests/ref`).
- **Fonts are the system's own,** found through CoreText on macOS and
  fontconfig on Linux. The portable families map to them: on macOS,
  `mono` is Menlo, `sans` Helvetica, `serif` Times and `typewriter`
  Courier. Metrics are close to X's but not identical. A font name that
  isn't found fails, as under X.
- **Image files:** GIF, as in Icon, and also PNG, JPEG, BMP and QOI, for
  both `ReadImage` and `WriteImage`.
- **Not supported:** the numeric colour forms `rgb:r/g/b` and `RGBi:...`.
  `iconlabel=` is accepted but never shown, since modern desktops have
  no icon labels. Keypad keys report the characters they type, not
  keypad keysyms.

### The toolchain

- **There is no separate translator and linker** (icont), and nothing is
  written beside your sources. `gcon build` writes one executable file,
  which runs on the root's `gcon-vm`.
- **Memory:** gcon has one collected heap, not iconx's regions.
  `&allocated` counts the bytes allocated since the program started;
  `&regions` gives iconx's initial sizes and `&storage` gives zeros.
  iconx's memory variables (`BLKSIZE`, `STRSIZE`, ...) have no effect.
- **What is shipped:** gcon replaces *using* Icon, not Arizona's whole
  distribution. `lib/ipl` holds the IPL's procedures and include files,
  the programs Arizona builds into its `bin` (`colrbook`, `colrpick`,
  `fontpick`, `palette`, `vib`, `wevents`, `xgamma`, all built into the
  root's `bin`) and the C functions (`link cfunc`, as
  `lib/libcfunc.so`). The rest of the IPL is in
  [Arizona's repository](https://github.com/gtownsend/icon).

## Performance

These are micro-benchmarks from `vm/tests/bench.sh`, which builds each
loop with both systems and reports wall-clock nanoseconds per
iteration, best of three. They were run on an Apple M5 with Apple clang
21 on 2026-11-26.

| benchmark | iconx (ns) | gcon (ns) | gcon / iconx |
|---|---:|---:|---:|
| empty loop | 7.4 | 3.8 | 0.52 |
| `*s` | 9.7 | 3.8 | 0.39 |
| `x + 407` | 14.8 | 4.2 | 0.28 |
| `L[3]` | 14.6 | 4.3 | 0.29 |
| `p()` | 15.5 | 9.4 | 0.61 |
| `q(1, 2)` | 30.6 | 13.7 | 0.45 |
| `s ? upto(c)` | 39.7 | 16.3 | 0.41 |
| `s ? find(s2)` | 38.5 | 21.2 | 0.55 |
| `move(1)` | 35.1 | 13.3 | 0.38 |
| `left("a", 10)` | 25.5 | 22.9 | 0.90 |
| `map(...)` | 27.3 | 23.0 | 0.84 |
| table insert and delete | 36.9 | 19.8 | 0.54 |
| `pop(push(L, 0))` | 27.1 | 14.3 | 0.53 |
| `@C`, a co-expression switch | 2,538 | 12.5 | 0.005 |
| `@(create 1)` | 17,636 | 41.5 | 0.002 |

Leaving out the co-expressions, the geometric mean is **0.49** of
iconx's time. iconx gives each co-expression its own C stack and
switches between them in assembly. gcon's VM is not recursive, so a
co-expression is just a VM state, and a switch costs about as much as a
procedure call.

On a whole program, a signal-processing application (records in a big
list, field access in a tight loop) runs its main pass over 730,000
samples in 1,265 ms, against iconx's 2,710 ms (0.47). A built "hello,
world" starts in 6.6 ms, against iconx's 11.2.

## How it is checked

Icon's own C sources are the authority on the language, so gcon is
checked against Arizona's icont and iconx:

- **The language:** 88 of the 89 programs in Icon's test suite give
  iconx's output exactly (`vm/tests/conformance.sh`); the other,
  `ilib`, differs only in a line that depends on the clock.
- **The front end** (preprocessor, parser and name resolution) is
  compared with icont itself on every Icon file in Arizona's source tree
  (`src/tests/oracle.sh`).
- **Graphics** are compared pixel for pixel with Arizona Icon's X11
  images (`ref.sh`) and with icon-sdl's iconx, which uses the same
  backend (`iconx.sh`). They are also run in real windows, with scripted
  keys and clicks (`window.sh`).
- The runtime is tested under the address and undefined-behaviour
  sanitizers.

For gcon's own additions (`fmt`, `lsp`, `vet` and the Unicode
behaviour), gcon's recorded results are the reference: golden files,
and runs of 318 programs from the test suite and the IPL
(`vm/tests/expected.sh`). See [docs/development.md](docs/development.md).

## Layout

| Path | Contents |
|---|---|
| `src/` | the `gcon` command: preprocessor, parser, compiler, driver, `fmt`, `lsp`, `vet` ([src/README.md](src/README.md)) |
| `vm/` | `gcon-vm`, the runtime ([vm/README.md](vm/README.md)) |
| `graphics/` | `gcon-graphics`, the display server, with SDL3 and Blend2D vendored and pinned ([graphics/README.md](graphics/README.md)) |
| `lib/ipl/` | the Icon Program Library, as gcon ships it ([lib/ipl/README](lib/ipl/README)) |
| `editors/` | editor support: the tree-sitter grammar, and the Zed and VS Code extensions ([editors/README.md](editors/README.md)) |
| `docs/` | [design.md](docs/design.md) (decisions and their reasons), [development.md](docs/development.md) (building and testing), [syntax.md](docs/syntax.md) |
| `experiments/` | Go-style channels on co-expressions, a Tcl/Tk binding |
| `bin/`, `libexec/`, `lib/libcfunc.so` | what `build.sh` builds |

Builds never use the network. The display server's third-party sources
are in `graphics/deps`, pinned by SHA-256, and only `deps/update.sh`
downloads them.

## History

gcon was written first in Go. It was then ported to C part by part, and
each part was proved to give the same results as the Go before the Go
was removed. The Go is kept as the tag `go-reference`. The `g` came from
Go; it can stand for graphics now. The front end began as a fork of
[icontools](https://github.com/phonologus/icontools), which remains a
standalone formatter and language server for Arizona Icon.

## Licence

gcon is under the MIT licence; see [LICENSE](LICENSE). The Icon Program
Library in `lib/ipl` is in the public domain; [NOTICE](NOTICE)
reproduces the copyright notices on the card bitmaps in
`lib/ipl/gprocs/cardbits.icn`, as a precaution. The display server's
vendored libraries keep their own licences (`graphics/deps`).
