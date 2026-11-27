# Gcon documentation

Gcon is an implementation of the [Icon](https://www.cs.arizona.edu/icon/)
programming language, version 9.5, written in C: a translator, a virtual
machine and a graphics display server, with modern tools in one `gcon`
command. Icon 9.5 programs run unchanged, about twice as fast as under
iconx, and graphics draw with SDL3 and Blend2D instead of X11.

This is the documentation of the Gcon you have installed. `gcon doc`
opens it in your browser; `gcon doc NAME` opens one page, and `gcon doc
-text NAME` prints it. Served by `gcon doc`, every example program can
be edited and run where it stands.

## Using Gcon

- [Getting started](start.md): building Gcon, the root, running and
  building programs
- [Editors](editors.md): the language server, and the Zed and VS Code
  extensions
- [Doc comments](comments.md): documenting your own procedures, for
  `gcon doc` and your editor
- [The commands](help/index.md): `gcon help` and a page for each command

## The language

- [Differences from Icon 9.5](icon95.md): what Gcon adds, and what it
  changes
- [Strings and Unicode](unicode.md): bytes, csets and usets
- [Graphics](graphics.md): the display server, fonts, images and keys
- [Environment variables](help/environment.md)
- [Where linked files are found](help/library.md)
- [The reference](ref/index.md): the built-in functions and keywords

## About Gcon

- [Performance](performance.md): measurements against iconx
- [How Gcon is checked](checking.md), its history and licence

## Internals

For the curious, and for anyone building on Gcon: another display
server, a tool that reads built programs.

- [How Gcon is built](internals/index.md): the parts and how they meet
- [The built program](internals/program.md): the file `gcon build`
  writes
- [The instructions](internals/instructions.md): gcon-vm's instruction
  set
- [The virtual machine](internals/vm.md): values, frames, generators,
  co-expressions and the collector
- [C functions](internals/loadfunc.md): `loadfunc` and the C interface
- [The graphics protocol](internals/protocol.md): between gcon-vm and the
  display server
