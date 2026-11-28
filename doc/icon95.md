# Differences from Icon 9.5

Icon 9.5 programs behave as they do under iconx. What Gcon adds to the
language is small:

- usets, written `u'...'`, and the keyword `&uset`
- ranges in cset and uset literals: `'a..z'`
- `\u` and `\U` escapes, after Go's, in any literal: `"caf\u00e9"`
- four built-ins: `uchar`, `uord`, `uset` and `uvalid`
- the library module `uclass`

[Strings and Unicode](unicode.md) describes them. Only two can change
what a 9.5 program does, and none in Icon's tests or library does: a
literal with `\u` (in 9.5, the letter `u`), and `image` of UTF-8 text,
which Gcon shows as itself. `gcon vet -icon95` reports any use of
Gcon's additions, for code that must also run under Arizona Icon.

The other differences are in graphics, which no longer uses X11
([Graphics](graphics.md)), and in the toolchain.

## The toolchain

- **There is no separate translator and linker** (icont), and nothing is
  written beside your sources. `gcon run` translates and runs a program;
  `gcon build` writes one executable file, which runs on the root's
  `gcon-vm` ([the built program](internals/program.md)).
- **Linking is from source.** `link name` finds `name.icn`, not
  `name.u1`; [where linked files are found](help/library.md) gives the
  search order. A file that declares `main` is passed over, as icont
  passes over a source file that was never translated.
- **Memory:** Gcon has one collected heap, not iconx's regions.
  `&allocated` counts the bytes allocated since the program started;
  `&regions` gives iconx's initial sizes and `&storage` gives zeros.
  iconx's memory variables (`BLKSIZE`, `STRSIZE`, ...) have no effect,
  and there is no limit on co-expressions (`COEXPSIZE`).
- **`&version`** is `"Icon Version 9.5 (Gcon)"`, and `&features`
  includes `SDL graphics` in place of `X Windows`.
- **What is shipped:** Gcon replaces *using* Icon, not Arizona's whole
  distribution. `lib/ipl` holds the IPL's procedures and include files,
  the programs Arizona builds into its `bin` (`colrbook`, `colrpick`,
  `fontpick`, `palette`, `vib`, `wevents`, `xgamma`, all built into the
  root's `bin`) and the C functions (`link cfunc`, as
  `lib/libcfunc.so`). The rest of the IPL is in
  [Arizona's repository](https://github.com/gtownsend/icon).
