# Gcon

Gcon is an implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language, version 9.5, written in C. It has a translator,
a virtual machine and a graphics display server. One script builds the
lot, and there is no `make Configure` step. Compared with Arizona's
icont/iconx, it:

- **Runs Icon 9.5 programs unchanged.** They give iconx's output on Icon's
  own test suite, and the Icon Program Library runs, graphics included.
- **Runs about twice as fast** as iconx, and co-expressions run hundreds
  of times faster ([Performance](doc/performance.md)).
- **Uses a modern graphics backend:** SDL3 and Blend2D, in place of X11,
  with antialiasing, HiDPI and native windows.
- **Keeps Icon 9.5's strings and csets,** which are bytes, and adds
  usets for UTF-8 text, kept apart from them
  ([Strings and Unicode](doc/unicode.md)).
- **Comes with modern tools:** one `gcon` command that runs, builds,
  formats and checks programs, serves as a language server, and carries
  its own manual and documentation.

```
$ cat hello.icn
procedure main()
   write("h", uchar(233), "llo, wörld: ", *"wörld", " bytes")
end
$ gcon run hello.icn
héllo, wörld: 6 bytes
```

## Quick start

You need a C compiler. For windows, `./build.sh` builds the platform's
own display server: on macOS the Cocoa one, which needs nothing more;
elsewhere the X11 one, which needs Xlib (`libx11-dev` on Debian and
Ubuntu, `libX11-devel` on Fedora). Without either, everything except
windows works.

```sh
git clone https://github.com/phonologus/gcon
cd gcon
./build.sh                          # builds into the checkout; no network
export PATH=$PWD/bin:$PATH          # the only set-up needed
gcon run hello.icn
```

- On **macOS**, install the Xcode Command Line Tools.
- `./build.sh --withX11` builds the X11 server instead (on macOS under
  XQuartz), for Arizona Icon's exact pictures; `--withSDL` the portable
  SDL3 + Blend2D server, which also needs a C++ compiler and CMake (the
  packages are listed in `graphics/linux-packages.txt` for Debian and
  Ubuntu, `graphics/linux-packages-fedora.txt` for Fedora).
  [Graphics](doc/graphics.md) says more.

```sh
gcon run prog.icn [args]            # translate and run; nothing is written
gcon build prog.icn                 # write ./prog, a runnable program
gcon fmt -w *.icn                   # format sources in place
gcon vet prog.icn                   # report likely mistakes
gcon lsp                            # language server, for your editor
gcon help [command | topic]         # the manual
gcon doc [name]                     # the documentation, in your browser
```

## Documentation

`gcon doc` opens the documentation of the Gcon you have built in your
browser, and `gcon help` is the manual of the commands. The same pages
are here, in [doc](doc/index.md):

- [Getting started](doc/start.md) and [editors](doc/editors.md)
- [Differences from Icon 9.5](doc/icon95.md),
  [strings and Unicode](doc/unicode.md), [graphics](doc/graphics.md)
- [Performance](doc/performance.md) and
  [how Gcon is checked](doc/checking.md)
- [Internals](doc/internals/index.md): the built program, the
  instructions, the virtual machine, C functions and the graphics
  protocol

Working on Gcon itself: [design/development.md](design/development.md)
(building, testing, releases) and [design/design.md](design/design.md)
(the decisions and their reasons).

## Licence

Gcon is under the MIT licence; see [LICENSE](LICENSE). The Icon Program
Library in `lib/ipl` is in the public domain; [NOTICE](NOTICE)
reproduces the copyright notices on the card bitmaps in
`lib/ipl/gprocs/cardbits.icn`, as a precaution. The display server's
vendored libraries keep their own licences (`graphics/deps`), and the
Unicode Character Database in `deps/unicode` is under the Unicode
License v3 (`deps/unicode/LICENSE`).
