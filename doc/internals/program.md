# The built program

`gcon build` writes a program as one executable file; `gcon run`
writes the same encoding to a temporary file and hands it to gcon-vm.
This page describes the encoding, format version 3: what the front end
writes (`src/compile.c`, `src/driver.c`) and what gcon-vm's loader
reads (`vm/src/load.c`), both defined in `vm/src/format.h`. [The
instructions](instructions.md) describes the code in it, and `gcon dis
FILE` lists any program, source or built, in full.

## The file

A built program is a shell script followed by the encoding:

```
#!/bin/sh
GCBIN='/home/you/gcon/bin/gcon'
GCVM='/home/you/gcon/libexec/gcon-vm'
case "$0" in */*) GCLCL="${0%/*}/gcon" ;; *) GCLCL=gcon ;; esac
[ -n "$GCONVM" ] && exec "$GCONVM" "$0" "$@"
[ -n "$GCON" ] && exec "$GCON" run "$0" "$@"
[ -x "$GCLCL" ] && exec "$GCLCL" run "$0" "$@"
[ -x "$GCVM" ] && exec "$GCVM" "$0" "$@"
[ -x "$GCBIN" ] && exec "$GCBIN" run "$0" "$@"
exec gcon run "$0" "$@"
[gcon program follows]
```

The script runs the program on a gcon-vm directly where it can, so as
not to start `gcon`: `$GCONVM`; else `$GCON`, or a `gcon` beside the
program; else the gcon-vm of the root that built it (`GCVM`), or the
`gcon` that built it (`GCBIN`); else `gcon` on the `PATH`. A file is
taken as built when it starts `#!` and a line `[gcon program follows]`
is followed by the magic below, so `gcon run` accepts either source or
a built program; gcon-vm takes a built program, or a file that is the
bare encoding (as `gcon run` hands it over).

## The encoding

All integers are varints, as Go's `encoding/binary` writes them:
unsigned, seven bits to a byte, least significant first, the high bit
set on every byte but the last. A signed varint is zigzag-encoded (0,
-1, 1, -2, ... as 0, 1, 2, 3, ...). A string is a varint length and that
many bytes.

1. **Magic**, eight bytes: `\x89GCN\r\n\x1a\n`, PNG's device: a first
   byte that is not ASCII, and line endings that a text-mode copy would
   mangle.
2. **Format version**, a varint: 3. gcon-vm reads exactly one version
   and refuses any other with "program built by a newer (older) Gcon
   ...: rebuild it". The version changes with any change to the
   encoding, the instruction set or what an instruction does.
3. **Sections**, each a four-byte tag, a varint length and that many
   bytes. As in PNG, a tag whose first letter is upper case is
   required: a gcon-vm that does not know it refuses the file. One with
   a lower-case first letter may be skipped, so information can be
   added without invalidating built programs. The required sections
   come in this order, then any optional ones, and `End ` last:

| Tag | Holds |
|---|---|
| `Bltn` | the built-ins the program uses |
| `Recd` | the record types |
| `Proc` | the procedures |
| `Cnst` | the constants |
| `Glob` | the globals, main and the invocable set |
| `info` | build information (optional) |
| `End ` | the CRC-32 (IEEE) of every byte before it, from the magic, four bytes little-endian |

Nothing may follow `End `. A section must be read exactly: bytes left
over are an error.

## The sections

**`Bltn`**: a count, then for each built-in a kind byte and a name. The
kinds: 0 an operator (`+`, `u-`, `[]`, `to`, ...), 1 a function
(`write`), 2 an assignment operator (`:=`, `+:=`, `<-`), 3 `!` applied
to a procedure (name empty), 4 a field reference (the field's name), 5
an operation gcon-vm does not provide (an error if run). A name gcon-vm
does not know is refused ("built by a newer Gcon?"). Instructions and
values refer to built-ins by their index here.

**`Recd`**: a count, then for each record type its name, and a count
and the field names.

**`Proc`**: a count, then for each procedure:

- its name and the file it was declared in;
- the number of parameters, and a byte: 1 if the last collects the
  rest of the arguments as a list (`p(a, b[])`), else 0;
- the number of locals (parameters first, then locals), and a count and
  the locals' names (for tracebacks and `variable`);
- a count and the indices of the globals that hold its statics;
- a count and the names of other files its code came from (through
  `$include`);
- the number of temporaries;
- a count and the instructions.

Each instruction is an opcode byte, a flags byte, the change in source
line from the previous instruction (a signed varint), then the fields
its flags name, in this order:

| Flag | Field | Encoded as |
|---|---|---|
| 1 | `dst` | an operand |
| 2 | `x` | an operand |
| 4 | `h` | a varint: a temporary, the handle for resumption |
| 8 | `args` | a count and that many operands |
| 16 | `fn` | a varint: a built-in's index in `Bltn` |
| 32 | `l1` | a varint: an instruction index, the success label |
| 64 | `l2` | a varint: an instruction index, the failure label |
| 128 | `file` | a varint: 0 the procedure's file, k its k-th other file |

An operand is a varint `index << 3 | kind`, the kind 1 a temporary
(written `t2` in listings), 2 a local (`l0`), 3 a global (`g1`) or 4 a
constant (`k0`). An absent field is zero.

**`Cnst`**: a count, then the constants, each a value. A value is a tag
byte and its data:

| Tag | Value | Data |
|---|---|---|
| 0 | `&null` | none |
| 1 | an integer | a signed varint |
| 2 | a large integer | a sign byte (1 negative), a count and the magnitude's bytes, most significant first |
| 3 | a real | eight bytes, the IEEE double, little-endian |
| 4 | a string | a string |
| 5 | a cset | 32 bytes: four 64-bit words, little-endian, bit c for byte c |
| 6 | a built-in | a varint, its index in `Bltn` |
| 7 | a procedure | a varint, its index in `Proc` |
| 8 | a record constructor | a varint, its index in `Recd` |
| 9 | a uset | a cset's 32 bytes (the ASCII characters and the bytes from 0x80), then a count and that many ranges of code points from 0x80, each two varints, low and high: sorted, disjoint, not adjacent, no surrogates |

**`Glob`**: a count, then for each global its name and its initial value
(a procedure, built-in or record constructor; `&null` for a variable).
Then main's index as a signed varint (-1 for none), and a byte: 1 if a
bitmap of the globals that string invocation may reach follows (one bit
each, least significant first; `invocable all` sets them all), else 0.

**`info`**: a count, then pairs of strings: `gcon`, the version that
built the program, and `predefined`, the preprocessor symbols it was
built with. gcon-vm uses one thing from it: a program whose `predefined`
lacks `_GRAPHICS` was built with `gcon build -nographics`, and runs as
under an Icon built without graphics (no graphics functions; `open`
fails for a window; `&window`, `&x` and the other graphics keywords
fail). A program without the section has graphics.

## What the loader checks

Before a program runs, gcon-vm checks every index: operands within the
procedure's temporaries and locals and the program's globals and
constants, labels within the procedure, statics, files and handles
within range; and that each instruction's operands are of the kinds the
interpreter assumes (a destination that is a temporary, `once`'s operand
a global, and so on). A program that fails is refused as "damaged
program", so the interpreter itself checks nothing.

## gcon-vm's command line

```
gcon-vm [-progname name] program [arguments]
gcon-vm -dis program
gcon-vm -version
```

`gcon run` translates a source file, writes the encoding to a temporary
file it deletes at once, and runs `gcon-vm -progname NAME /dev/fd/N`, so
that `&progname` is what the built program would be called.
