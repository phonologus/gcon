# Getting started

## Building Gcon

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

Builds never use the network: the display server's libraries and the
Unicode database are in the checkout, pinned by their SHA-256.

## The root

The checkout becomes a *Gcon root*, in the way a Go checkout is a
GOROOT: it holds the `gcon` command (`bin`), the runtime and display
server (`libexec`), the Icon Program Library (`lib/ipl`), Gcon's own
library (`lib/gcon`) and this documentation (`doc`). `gcon` finds the
root from its own location, so you don't need `GCONROOT` or any other
variable. `gcon env` shows where Gcon looks.

The Icon Program Library is part of the root: `link graphics` just
works. `IPATH` and `LPATH` still add directories to search, as with
icont; [where linked files are found](help/library.md) has the details.

## Running and building programs

```sh
gcon run prog.icn [args]            # translate and run; nothing is written
gcon build prog.icn                 # write ./prog, a runnable program
gcon fmt -w *.icn                   # format sources in place
gcon vet prog.icn                   # report likely mistakes
gcon lsp                            # language server, for your editor
gcon help [command | topic]         # the manual
gcon doc [name]                     # this documentation
```

No `.u1` or `.u2` files are written anywhere: programs are linked from
source each time, which takes a few milliseconds. A built program is
one executable file, which runs on the root's runtime.

```
$ cat hello.icn
procedure main()
   write("h", uchar(233), "llo, wörld: ", *"wörld", " bytes")
end
$ gcon run hello.icn
héllo, wörld: 6 bytes
```

Each command has a page: [gcon help](help/index.md) lists them.
