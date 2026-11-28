# How Gcon is built

Gcon is three programs and a library, in a root directory:

| Part | Is | Source |
|---|---|---|
| `bin/gcon` | the command: translator, driver, formatter, language server, `vet`, `help`, `doc` | `src/` |
| `libexec/gcon-vm` | the runtime: loads a built program and runs it | `vm/src/` |
| `libexec/gcon-graphics` | the display server: windows and drawing | `graphics/src/` |
| `lib/ipl`, `lib/gcon` | the Icon Program Library, and Gcon's own | |

All are C (the display servers use the platform's own libraries: AppKit,
Xlib, SDL3), built by one `build.sh`, which needs no network.

## From source to running program

1. **The front end** (`gcon`): the preprocessor (icont's own `ipp.c`),
   the lexer, the parser and name resolution, all without recursion, so
   no program is too deeply nested to translate. Errors are icont's,
   in icont's words, checked against icont itself on every Icon file
   in Arizona's tree.
2. **Linking** is from source: each `link` is found and translated in
   the same run ([where linked files are found](../help/library.md)).
3. **The compiler** translates goal-directed evaluation into jumps,
   each expression with its success and failure continuations, and
   writes [the built program](program.md): procedures of
   [instructions](instructions.md), constants and globals.
4. **gcon-vm** loads the program, checks every index in it, and runs it
   ([the virtual machine](vm.md)). `gcon run` hands the program over
   through a temporary file it deletes at once; `gcon build` writes it
   after a shell script that finds a gcon-vm.
5. **gcon-graphics** is started by gcon-vm when a program first opens a
   window, as a child process on a socket pair; gcon-vm keeps Icon's
   graphics semantics and sends it drawing requests in
   [the graphics protocol](protocol.md).

## Why separate programs

The runtime is separate from the command so that a built program needs
only gcon-vm, which starts in a few milliseconds, and so that each can
change without the other; the program format's version is checked on
every load. The display server is separate so that gcon-vm needs no
windowing libraries, a program that draws nothing never loads
them, and another server (on another platform, or for a terminal) can
take its place by speaking the protocol. The reasons are recorded in
Gcon's design notes (`design/design.md`).
