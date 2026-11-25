# gcon design notes

gcon runs Icon 9.5 programs on a virtual machine written in Go. It is one
binary: it translates Icon source to gcon's own bytecode and executes it.
It supports Go 1.24 and later.

Compatibility target: the *language* of Icon 9.5, checked against the
reference implementation. Compatibility with icont's ucode/icode files
is not a goal; gcon has its own bytecode.

## Scope and direction

gcon is meant to become a standalone Icon toolchain in the Go mould:
`go install` puts one `gcon` command on the path, and that command
gathers the tools under subcommands (run, build, fmt, the language
server, ...). Over time it takes in what now lives in separate projects:
the formatter and language server (icontools), the tree-sitter grammar
and editor extensions, and a graphics back-end, possibly based on Fyne.

icontools and icon-sdl remain standalone, for the wider Icon community,
built around the reference implementation. gcon does not depend on them.
It forked the icontools front end (lexer, parser, name resolution,
built-in catalogue, and the icont oracle) at icontools 9cf9615, and the
two evolve independently. Syntax facts are in [syntax.md](syntax.md).

Dependencies: the core (front end, translator, VM, and tools that work on
source) uses only the standard library. A graphics back-end such as Fyne
is a large dependency that brings in cgo, so it will need a boundary
(a separate module or a build tag) that keeps the core installable
without it. That is to be decided when graphics work starts.

## Prior art

### iconx 9.5

- The interpreter suspends a generator by **calling `interp()` recursively**,
  so a suspended generator's state lives on the C stack. Built-in generators
  (`!`, `find`, `upto`, ...) are C functions that suspend the same way.
- Because of that, each co-expression needs its own C stack. In 9.5 that
  stack comes from a **pthread**: `src/common/rswitch.c` creates one thread
  per co-expression, and every switch is a semaphore post plus a semaphore
  wait (named semaphores on macOS). Earlier versions used per-platform
  assembly to swap stack pointers.

### Jcon (Proebsting and Townsend)

Jcon translates Icon to the JVM using the translation in Proebsting's
"Simple Translation of Goal-Directed Evaluation" (PLDI 1997). It
implemented co-expressions with Java threads.

### Goaldi (Proebsting and Townsend)

Source: github.com/proebsting/goaldi (examined at commit 983d69b, January 2025).
Goaldi is an Icon-like language, not compatible with Icon, whose
interpreter is written in Go.

- The translator is written in Goaldi itself (`tran/*.gd`, bootstrapped).
  It emits an IR based on the Proebsting translation, as labelled chunks
  of instructions (`ir/structs.go`: `Ir_Goto`, `Ir_IndirectGoto`,
  `Ir_Succeed{ResumeLabel}`, `Ir_Fail`, `Ir_Call`, `Ir_ResumeValue`, ...).
- Goal-directed control *within* a procedure is entirely gotos between
  chunks: no recursion.
- Procedure results are a pair `(Value, *Closure)`: failure is `(nil, nil)`,
  return is `(v, nil)`, and suspension is `(v, resume)`
  (`runtime/calling.go`). Frames are heap objects, so a suspended procedure
  survives in a closure rather than on the Go stack.
- Procedure *calls*, however, recurse on the Go stack (`execute` calls
  `Call` and then `execute` again). So code that is still running inside a
  call chain does need its own Go stack, and Goaldi's `create` starts a
  **goroutine** (`interp/execute.go`, `Ir_Create`). Co-expressions are
  reduced to channels: values go out but not in, and there is no symmetric
  transfer, `&source` or `^C` (`doc/diffs.adoc`, "Co-Expressions").

Takeaways: the Proebsting translation works well in Go, and it is what
gcon's code generator should use. But Goaldi gave up exactly the part of
Icon that gcon has to keep: full co-expressions.

## Co-expressions

Measured 2026-11-25 on an Apple M-series Mac, Go 1.27, iconx 9.5 built
from the reference tree (`experiments/coswitch`):

| implementation                              | `@c` round trip | create + first `@` |
|---------------------------------------------|----------------:|-------------------:|
| iconx 9.5 (pthreads and semaphores)         |        2,600 ns |          18,400 ns |
| goroutine and channel handoff (symmetric)   |   188 ns (135 ns with GOMAXPROCS=1) | 844 ns |
| `iter.Pull` (runtime coroutine, asymmetric) |           30 ns |             144 ns |
| explicit-stack VM (toy lower bound)         |           ~1 ns |              46 ns |

The iconx run spent 80% of its time in the kernel (2.4M involuntary
context switches per 1M activations).

Goroutines would be an easy and large improvement over 9.5, but they bring
problems of their own:

- Icon transfer is symmetric (`v @ &source`, or transfer to a third
  co-expression). `iter.Pull` cannot express that, so the general case
  needs channels.
- Parked goroutines are never garbage-collected. An unreachable
  co-expression would need a `runtime.AddCleanup` hook to wake its
  goroutine and unwind it.
- A runtime error inside a co-expression's goroutine has to be recovered
  and handed to `&main`.

Measured again on gcon's real VM loop once co-expressions were built
(2026-11-25, same machine; `experiments/coswitch/vmswitch.icn`, timed
externally, per operation including about 10 ns of loop overhead):

| implementation | `@c` round trip | `@(create 1)` |
|----------------|----------------:|--------------:|
| iconx 9.5      |        2,520 ns |     17,750 ns |
| gcon           |           30 ns |        150 ns |

### Decision: a non-recursive VM

gcon's VM never recurses on the Go stack for Icon-level control:

- Goal-directed evaluation within a procedure compiles to jumps, using
  the Proebsting translation as Goaldi does.
- Procedure call, return, suspend, resume and fail push and pop
  heap-allocated frames on an explicit stack owned by the VM loop.
- Built-in generators are resumable Go values (state machines with a
  `Next` method), not functions that yield through callbacks. No Icon 9.5
  built-in calls back into Icon code, so nothing forces re-entry into the
  interpreter.
- A co-expression is a frame stack plus its activator and the template
  that `^C` (refresh) needs. `@` switches the VM's current stack. That
  gives value transmission, symmetric transfer, `&source`, `&current`,
  `&main` and `^C` at the cost of a pointer swap. The garbage collector
  reclaims unreachable co-expressions, and there is no `COEXPSIZE` limit.

Cost: built-in generators are more work to write than yield loops. That
is the deliberate trade.

## Strings and Unicode

Icon 9.5 strings are bytes, and a cset is a set of 256 byte values.
gcon makes Unicode text the default while keeping byte strings, and follows
Go's handling of UTF-8 throughout. All encoding and decoding goes through
`unicode/utf8`; gcon never hand-rolls a UTF-8 decoder.

### Prior art

- **Goaldi** makes every string Unicode, stored as parallel low-byte and
  high-bits arrays (`runtime/vstring.go`). It has no byte strings (`readb`
  puts one byte in each character), and it **drops string scanning and
  csets** (`doc/diffs.adoc`). Its source names "Go strings with
  annotations for quicker `*s`, `s[i]`" as the alternative it did not
  take; gcon takes it.
- **Object Icon** (objecticon.sourceforge.net, "Unicode") adds a `ucs`
  type: a UTF-8 string with an offset index for random access, written
  `u"..."`. Its csets range over 0 to 0x10FFFF, and it adds `&uset`,
  `uchar` and `ucs()`. Unicode is opt-in there (a `u` prefix, plus
  `$encoding UTF-8`), and mixing a `ucs` with an invalid string is a
  runtime error.

### Decision: one representation, two units

- A string value is a Go `string` plus a **unit**, text or bytes. Length,
  subscripts, `!s`, `move`, `&pos`, `left`/`right`/`center`, `reverse` and
  `map` all count in the value's unit.
- **Malformed UTF-8 is handled as Go handles it.** A text string may hold
  any bytes, and they are never rewritten. Reading it as characters
  follows `utf8.DecodeRuneInString`: each invalid byte is a one-byte
  character whose code point is U+FFFD. So `*s` is
  `utf8.RuneCountInString`, and `!s` yields exactly what a Go `range`
  loop does. Since `s[i]` is a substring of the original bytes, `s` is
  always the concatenation of `!s`, and any data round-trips unchanged.
  Joining two text strings can complete a split sequence, so
  `*(a || b)` may be less than `*a + *b`. That happens only with invalid
  UTF-8, as it does in Go.
- **Conversions never fail or copy.** `runes(s)` and `bytes(s)` relabel
  the same bytes, as Go's `[]rune` and `[]byte` conversions read them. `utf8valid(s)` is available for programs that need to check.
- **ASCII is neutral.** A cached flag marks all-ASCII strings, which
  behave identically in either unit and take a fast path. Every ASCII-only
  9.5 program therefore behaves as in 9.5.
- **Mixed operands.** When an operation combines units, non-ASCII text
  makes the result text. Otherwise non-ASCII bytes make it bytes.
  Otherwise the result is ASCII. Nothing is an error. For example
  `"abc" || char(200)` is bytes, exactly as in 9.5.
- **Literals.** A literal's unit comes from its contents. It is text if it
  contains a non-ASCII code point (a UTF-8 character in the source, or a
  `\u`/`\U` escape of 0x80 or more); any `\x80`–`\xff` escapes in it are
  then lone bytes inside the text. Otherwise it is bytes if it contains
  such a byte escape or source bytes that are not valid UTF-8 (a Latin-1
  file), so such files behave as in 9.5. Otherwise it is ASCII. No literal
  in the reference tree or in `icn/` mixes raw UTF-8 with byte escapes
  (none has raw UTF-8 at all; 14 have Latin-1 bytes), so the mixed rule
  changes nothing there. Escapes follow Go:
  `\x` gives a byte, and `\u` (4 hex digits) or `\U` (up to 6) gives a code
  point. A surrogate or a value above 0x10FFFF is a compile-time error.
  In 9.5 `\u` is not an escape: icont reads `"\u00e9"` as the five
  characters `u00e9`. No file in `icon/tests` or `icon/ipl` uses `\u` or
  `\U` in a literal, so the incompatibility is accepted.
- **Comparison is plain byte comparison.** For valid UTF-8, byte order is
  code-point order, so `<<`, `==` and `sort` need no decoding.
- **Code points to characters.** `char(i)` keeps its 9.5 meaning for
  0–255: text below 128, bytes from 128 to 255. `uchar(i)` gives a
  one-character text string for any code point; an invalid code point
  gives U+FFFD, as Go's `string(rune)` does. `ord` returns the code point,
  which is U+FFFD for an invalid byte.
- **I/O.** Files are text by default; mode `"b"` reads bytes. Reading does
  no validation or rewriting; it only sets the unit. In text mode
  `reads(f, n)` reads n characters and never splits a sequence.
- **Csets** have no unit; their characters are ASCII, bytes and code
  points (see "Csets" below).
- **Fast positions.** A scanning environment caches its cursor
  (`&pos` and the matching byte offset), so scanning functions cost only
  the distance they move. Random subscripts on non-ASCII text use a
  sparse offset index built on first use, one checkpoint every 32–64
  runes, as Object Icon does. ASCII strings need neither.
- Out of scope: normalization, grapheme clusters and display width. The
  unit is the rune, as in Go.
- **UTF-8 is the only text encoding.** As in Go, text means UTF-8 and
  anything else is binary. gcon does not transcode or guess encodings:
  a Latin-1 (or other) file read in text mode keeps its bytes, and its
  non-UTF-8 bytes read as U+FFFD; to work with it, read it as bytes.
  Latin-1 bytes in source literals make those literals bytes, which is
  what preserves 9.5's behaviour for them, not support for Latin-1 text.

### Csets

**Decision (revised 2026-11-25): csets have no unit.** There is one
universe of characters:

- the 128 ASCII characters;
- the 128 non-ASCII **bytes**, 0x80–0xFF; and
- the non-ASCII **code points**: 0x80–0x10FFFF less the 2,048 surrogates.

That makes 1,112,192 characters. A cset is a set of them. The byte 0xE9
and the code point U+00E9 are different members: `'\xe9é'` has two. Since
there is a single universe, csets form a Boolean algebra: `~~c === c`, De
Morgan's laws hold, `a -- b === a ** ~b`, and `===` is equality of
members, so identical csets behave identically. `TestCsetAlgebra` checks
these laws over a range of csets.

(The first design gave each cset a unit, text or bytes, with ASCII-only
csets neutral and complement taken over the cset's own unit. That broke
the laws: `~&cset` was the empty cset, which is neutral, so `~~&cset` was
`&uset`. Keeping a unit even on ASCII-only csets would restore
`~~c === c`, but then two `===` csets could have different complements.
The problem was having two universes.)

- **Matching.** Scanning functions read the subject's characters in the
  subject's unit. In a text subject, a character is a code point or an
  invalid byte (as `utf8.DecodeRuneInString` splits it), and matches
  when it is a member: an invalid byte matches only that byte member, and
  a real U+FFFD only the code point U+FFFD. In a bytes subject, a
  character is a byte, and matches when it is a byte member or a byte of
  a member code point's UTF-8 encoding: a bytes subject sees `'é'` as
  `{0xC3, 0xA9}`, which is what 9.5 makes of `'é'` in a UTF-8 source
  file.
- **The value is a string.** A cset's canonical string is its members
  sorted in string (`<<`) order and concatenated, a code point by its
  UTF-8 encoding and a byte as itself. Read as text, it is exactly its
  members: in sorted order a lone lead byte (0xC2–0xF4) is followed only
  by members whose first byte is at least as large, which is never a
  continuation byte, so it cannot fuse with them
  (`experiments/csetorder` checks this). Its unit is text if the cset has
  a code point, else bytes: the unit a literal with those members has.
  `string(c)` is that string, and `!c` generates the members in order.
  `string(&cset)` is 9.5's 256-byte string.
- `cset(s)` reads the characters of `s` in its unit: in text, valid
  sequences are code points and invalid bytes are bytes; in bytes, every
  byte is a byte.
- `runes(c)` is `c`. `bytes(c)` is the cset of the bytes that match `c`
  in a bytes subject: each code point is replaced by its encoding's
  bytes. So `bytes('é')` is `'\xa9\xc3'`.
- **Literals.** A cset literal's characters are read in the literal's
  unit, by the same rule as strings: in a text literal, `é` is a code
  point and `\xe9` a byte; in a bytes literal, everything is bytes.
- **Ranges** use Pascal's `..`: `'a..zA..Z_'`, `'\u0370..\u03ff'`,
  `'\x00..\x1f'`. A range is inclusive, and the two characters `..` can
  never be one byte or one code point. An unescaped `..` must have a
  member on each side; a `.` endpoint is written `\.` (which 9.5 already
  reads as `.`), so `'\...z'` is the range from `.` to `z`. A `..` at
  either end of a literal, or three unescaped dots, is a compile-time
  error. Both ends are code points (a range spanning the surrogates
  skips them) or both are bytes (`\x80..\xff`); a mixed or descending
  range is a compile-time error. In 9.5, `..` inside a cset literal only
  repeats the member `.`, and none of the 1,655 cset literals in the
  reference tree and `icn/` contains it, so no existing literal changes
  meaning. String literals do not have ranges.
- **Complement** is over the whole universe. `&cset` is ASCII plus the
  128 bytes, as in 9.5, so `~&cset` is every non-ASCII code point.
  `&uset` (Object Icon's name) is the whole universe, `~''`.
- **Representation.** The canonical string is the meaning, not the
  storage: `~'a'` would be about 4 MB as a string. Storage is a 256-bit
  set for ASCII and the bytes, and a sorted list of code-point ranges.
  The string is built only when asked for.
- **Unicode classes.** Go's `unicode` tables give csets for categories
  and scripts (names to be settled, e.g. `ucat("L")`, `uscript("Greek")`).

### image()

`image()` follows Go's `%q` (`strconv.Quote`): printable characters
appear as themselves, other code points as `\u`/`\U` escapes, and invalid
bytes as `\xNN`. Two Icon escapes are kept: `\e` for 0x1B and `\d` for
0x7F, where Go writes `\x1b` and `\x7f`. With those, `image()` of ASCII
matches 9.5 (checked against icont: `"\x00\x01\b\t\n\v\f\r\e\d"` renders
identically). 52 of the 99 reference tests call `image`.

- A **bytes** string escapes every byte 0x80–0xFF as `\xNN`, as 9.5 does.
  So the image shows the unit: `"é"` is text, while `"\xc3\xa9"` is the
  same bytes as a bytes string. Both images are literals that rebuild the
  value with its unit.
- Csets use single quotes and escape `'` rather than `"`, as 9.5 does. A
  cset whose members are those of a predefined cset images as its name
  (`&digits`, `&letters`, `&cset`, `&uset`, ...), as iconx's `csname`
  does.
- **Csets with ranges.** Otherwise a cset images by listing every member,
  as 9.5 does, when it has at most 256 members; that covers every 9.5
  cset. A larger cset images with ranges: code-point ranges first, then
  byte ranges, with runs of 4 or more consecutive members written `x..y`,
  shorter runs listed, and a `.` at either end of a range written `\.`.
  For example, `image(~&ascii)` is `'\u0080..\U10ffff\x80..\xff'`, and
  `image(~'a')` is `` '\x00..`b..\U10ffff\x80..\xff' ``.
- Since a cset has no unit, every cset's image is a literal that rebuilds
  it exactly: the literal's characters are read by the same rule that
  produced the image.
- **One inexact case, accepted.** A text string whose only non-ASCII
  content is invalid bytes (for example `"caf\xe9"`, from reading a
  Latin-1 file in text mode) has no literal of its own unit: its image
  reads back as bytes. The two differ only where units matter (`ord`
  gives 0xFFFD rather than 233, and cset matching). This is accepted,
  following Go: such a value is malformed text, and gcon does not try to
  make it first-class (see "UTF-8 is the only text encoding").

### Compatibility cost and open points

All 99 programs in `icon/tests/general` and their expected outputs are
pure ASCII. Of the 1,197 `.icn` files in `icon/tests` and `icon/ipl`, 19
contain non-ASCII bytes (14 of them valid UTF-8); it has not yet been
checked whether those bytes are in literals or only comments.

Differences from 9.5 are confined to complements, which now include the
code points. Matching is unaffected: a complement matches the same bytes
of a bytes subject as in 9.5. What differs is a complement's size, image,
string and elements. In the four reference tests that use characters
128–255:

- `collate.icn` and `iobig.icn` use only `&cset` (`string`, `reverse`,
  `left`, subscripts, `!&cset`), which is as in 9.5.
- `checkc.icn`: `&cset -- ~&ascii` gives `&ascii` and `image(&cset)` is
  unchanged, both as in 9.5. **Differs:** `&letters ++ ~&ucase` is the
  whole universe, `&uset`, not 256 characters.
- `scan.icn`: **differs** in `image(~&cset)`, which is
  `'\u0080..\U10ffff'`, not `''`; and in `image(~&ascii)`, which is
  `'\u0080..\U10ffff\x80..\xff'`, not the 128 bytes.

These three lines are kept deliberately and will be listed in the
conformance test with this reason.

## Implementation status

### Translation and VM

- The compiler (`internal/compile`) emits VM instructions directly from
  the syntax tree using Proebsting's translation, with no separate IR:
  each expression is compiled given its failure and success labels and
  returns its start and resume labels and its result operand. Labels may
  be aliases of other labels, resolved (with jump threading) when a
  procedure is finished. Code is never entered by falling into it.
- Operands are temporaries, locals, globals or constants. Locals and
  globals are read when the instruction using them runs, which gives
  Icon's late dereferencing (`write(x, x := 2)` writes `22`) at no cost.
  Expressions that produce variables (alternation, `if`, list elements)
  carry references: `*Value` for locals and globals, and `Var` values
  such as list elements.
- A co-expression (`internal/vm/coexpr.go`) is a frame chain plus its
  activator and state: not started, suspended after a result, waiting
  at its own activation, or exhausted. Its body is a separate region of
  the creating procedure's code, run in a new frame with copies of the
  creator's locals. One function, `transfer`, resumes a co-expression
  in whichever state it is, so activation, results, failure and
  symmetric transfer through `&source` are all the same operation.
- String scanning follows Jcon's translation: `OpScanEnter` saves the
  scanning environment in a temporary and `OpScanSwap` exchanges it with
  the current one. `&subject` and `&pos` are global, as in iconx:
  switching co-expressions does not switch them. Leaving scans early
  (`return`, `fail`, `suspend`, `break`, `next`) exchanges the
  environments of the scans being left, innermost first, as iconx's
  results show; `suspend` re-enters them when resumed.
- `break e` is resumable: a gate in the loop records e's resume label.
- Csets (`internal/vm/cset.go`) are a 256-bit set of ASCII and bytes plus
  sorted code-point ranges. Cset literals
  with `..` ranges are decoded by `lit.Cset`.
- gcon's extra built-ins (`runes`, `bytes`, `uchar`, `utf8valid`) and
  the keyword `&uset` are in the catalogue marked `Gcon`. Like any
  built-in, they are hidden by a declaration of the same name but not by
  an undeclared use, which would assign to the global. None of these
  names is used undeclared in the reference tree or `icn/` (`runes`,
  `uchar` and `utf8valid` do not occur at all). `text`, an earlier
  choice, was rejected: it is a natural variable name, used undeclared
  in `tests/general/diffwrds.icn`.
- Operators that produce at most one result are `OpOper` instructions
  calling a Go function; generators (`to`, `!`, built-in generators)
  and procedure calls use `OpCall` and `OpResume` with a handle in a
  temporary, which holds a suspended frame or a Go `Generator`.

### Deliberate simplifications (to be removed)

- Procedures return values, never variables.
- String subscripts are values, not assignable variables.
- Integers are 64-bit; overflow is run-time error 203 until large
  integers exist.
- Run-time errors print iconx's header lines but no traceback.
- Non-ASCII text positions are found from a cursor (the last position
  looked up in that string), so scanning is cheap; the sparse index of
  "Fast positions", for random access far from the cursor, is still to
  come.
- Not yet implemented: reals, records, sets, tables, programmer-defined
  control operations, files and input, most keywords and built-in
  functions, the preprocessor, `link`.

## Verification

- Oracle: `icon/tests/general` has 99 `.icn`/`.std` pairs. gcon's
  conformance test runs each program and compares its output with the
  `.std` file. Programs that do not yet pass are listed explicitly, so
  progress shows up in the diff.
- New syntax and semantics facts go here, with the C source or test that
  shows them.

## Plan

1. **Front end.** Done: forked from icontools 9cf9615, with its oracle
   tests passing.
2. **Vertical slice.** Done (see "Implementation status"): integers,
   strings, lists, locals, globals and statics, procedures and generators,
   the control structures, `write` and a few other built-ins. The string
   value carries its unit and ASCII flag from the start.
3. **Co-expressions.** Done: `create`, `@` with transmission, `^`,
   `*C`, `&current`, `&source` and `&main`, with symmetric transfer.
   Switch cost confirmed on the real VM loop (see "Co-expressions").
4. Grow the value types and built-ins, driven by the conformance suite.
5. `gcon build` producing a self-contained executable (the gcon binary
   with the bytecode appended), the counterpart of icont's output.
