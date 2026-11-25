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
- **Conversions never fail or copy.** `text(s)` and `bytes(s)` relabel the
  same bytes. `utf8valid(s)` is available for programs that need to check.
- **ASCII is neutral.** A cached flag marks all-ASCII strings, which
  behave identically in either unit and take a fast path. Every ASCII-only
  9.5 program therefore behaves as in 9.5.
- **Mixed operands.** When an operation combines units, non-ASCII text
  makes the result text. Otherwise non-ASCII bytes make it bytes.
  Otherwise the result is ASCII. Nothing is an error. For example
  `"abc" || char(200)` is bytes, exactly as in 9.5.
- **Literals.** A string literal is text, unless it contains a byte escape
  `\x80`–`\xff` or source bytes that are not valid UTF-8 (a Latin-1 file);
  then it is bytes, so such files behave as in 9.5. Escapes follow Go:
  `\x` gives a byte, and `\u` (4 hex digits) or `\U` (up to 6) gives a code
  point. A surrogate or a value above 0x10FFFF is a compile-time error.
  In 9.5 `\u` is not an escape: icont reads `"\u00e9"` as the five
  characters `u00e9`. No file in `icon/tests` or `icon/ipl` uses `\u` or
  `\U` in a literal, so the incompatibility is accepted.
- **Prefixed literals pin the unit.** `u"..."` is always text and
  `b"..."` is always bytes, whatever they contain. In `u"..."`, `\xNN` is a
  lone byte inside text, so every text string has a literal. In
  `b"..."`, `\u` and `\U` give the bytes of the code point's UTF-8
  encoding, as Go's string literals do. The same prefixes apply to cset
  literals, which also gain ranges (see "Csets"). The lexer reads `u` or `b`
  immediately followed by a quote as one token. In 9.5 an identifier
  directly before a literal is a syntax error, so no valid 9.5 program
  changes meaning. In the corpus it happens only in `tpp.icn`, which is
  only ever preprocessed (`icont -E`), and never with `u` or `b`.
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
- **Csets** carry a unit too (see "Csets" below).
- **Fast positions.** A scanning environment caches its cursor
  (`&pos` and the matching byte offset), so scanning functions cost only
  the distance they move. Random subscripts on non-ASCII text use a
  sparse offset index built on first use, one checkpoint every 32–64
  runes, as Object Icon does. ASCII strings need neither.
- Out of scope: normalization, grapheme clusters and display width. The
  unit is the rune, as in Go.

### Csets

A cset is a set of *characters*, in one unit, like a string. A
character is identified by its bytes, as a one-character substring is:

- In a **bytes cset**, a character is one byte. This is 9.5's cset.
- In a **text cset**, a character is either the UTF-8 encoding of a code
  point, or a **lone invalid byte** (0x80–0xFF), exactly as
  `utf8.DecodeRuneInString` splits a string. A lone invalid byte *reads*
  as U+FFFD (`ord` gives 0xFFFD) but is not rewritten: the cset
  containing byte 0xE9 is a different cset from the one containing byte
  0xFF, and from the one containing U+FFFD itself. Strings behave the
  same way.

A cset whose members are all ASCII is neutral, since ASCII characters
are the same in both units.

**The value is a string.** A cset is its members sorted in string (`<<`)
order and concatenated, together with its unit. That string is
canonical, and it reads back, in its unit, as exactly its members.
For text this holds because, in sorted order, a lone lead byte
(0xC2–0xF4) is followed only by members whose first byte is at least
as large, which is never a continuation byte, so it cannot fuse with
them. (`experiments/csetorder` checks this, and the bytes case below, on
random sets; it becomes a fuzz test of the real cset type.) Hence:

- `string(c)` is that string, and `!c` generates the members in order.
  `string(&cset)` is 9.5's 256-byte string.
- `cset(s)` reads the characters of `s` in its unit, then sorts and
  removes duplicates.
- **Relabelling reads the same bytes in the other unit**, exactly as for
  strings: `text(c)` and `bytes(c)` keep the stored bytes. Reading the
  sorted bytes of a bytes cset as text yields only lone invalid bytes,
  never a fused sequence (the same argument as above). So
  bytes → text → bytes is lossless: `bytes(text(c)) === c`.
  Text → bytes splits a multi-byte character into its bytes
  (`bytes('é')` is `{0xC3, 0xA9}`, which is what 9.5 makes of `'é'` in a
  UTF-8 source file). Going back to text then gives two lone bytes, not
  `é`. That loss cannot be avoided: a set of bytes has no order in which
  to remember that they formed a sequence.
- **Literals** follow the string rule: an unprefixed cset literal is text
  unless it contains a byte escape `\x80`–`\xff` or source bytes that are
  not valid UTF-8, in which case it is bytes. In an unprefixed literal
  `-` is an ordinary member, exactly as in 9.5.
- **Range literals.** Prefixed cset literals, `u'...'` (text) and
  `b'...'` (bytes), accept ranges: `u'a-zA-Z_'`, `u'\u0370-\u03ff'`,
  `b'\x00-\x1f'`. `x-y` is inclusive. `-` is a member when it comes first
  or last, or when it is written `\-`. In `u'...'` both ends must be code
  points (a range spanning the surrogates skips them) or both must be lone
  bytes (`\x80`–`\xff`). In `b'...'` both ends are bytes. A mixed or
  descending range is a compile-time error. Ranges cannot go in
  unprefixed literals: 22 of the 1,653 cset literals in the reference
  tree's 1,203 `.icn` files have `-` between two characters, e.g. `'+-0123456789'`, which
  as a range would include everything from `+` to `0`.
- **Set operations** (`++`, `**`, `--`) choose the result's unit as
  strings do (non-ASCII text, else non-ASCII bytes, else neutral). The
  other operand is first relabelled.
- **Matching.** Scanning functions read the subject's characters in the
  subject's unit and test each one's bytes for membership in the cset,
  relabelled to the subject's unit. A lone invalid byte in a text subject
  matches only that same byte; a real U+FFFD matches only U+FFFD.
- **Complement** is over the cset's own unit. For bytes, that is all 256
  bytes. For text, it is every code point that UTF-8 can encode
  (0–0x10FFFF less the 2,048 surrogates), plus the 128 lone invalid bytes:
  1,112,192 characters. A neutral cset complements as text, so
  `upto(~' ')` steps over `é` in text. `&cset` is the bytes cset of all
  256 bytes, so `~&cset` is `''` as in 9.5. `&ascii`, `&letters`,
  `&lcase`, `&ucase` and `&digits` are neutral. `&uset` (Object Icon's
  name) is the full text universe.
- **Representation.** The canonical string is the *meaning*, not
  necessarily the storage: `~'a'` would be about 4 MB as a string.
  Storage is a 256-bit bitmap for single-byte characters (ASCII, plus
  lone bytes in text or all bytes in bytes csets) and a sorted list of
  code-point ranges for multi-byte characters. The string is built only
  when asked for.
- **Unicode classes.** Go's `unicode` tables give text csets for
  categories and scripts (names to be settled, e.g. `ucat("L")`,
  `uscript("Greek")`).

### image()

`image()` follows Go's `%q` (`strconv.Quote`): printable characters
appear as themselves, other code points as `\u`/`\U` escapes, and lone
invalid bytes as `\xNN`. Two Icon escapes are kept: `\e` for 0x1B and
`\d` for 0x7F, where Go writes `\x1b` and `\x7f`. With those, `image()`
of ASCII matches 9.5 (checked against icont: `"\x00\x01\b\t\n\v\f\r\e\d"`
renders identically). 52 of the 99 reference tests call `image`.

- A **bytes** string or cset escapes every byte 0x80–0xFF as `\xNN`, as
  9.5 does. So the image shows the unit: `"é"` is text, while
  `"\xc3\xa9"` is the same bytes as a bytes string. Both images are
  literals that rebuild the value with its unit.
- A text string that holds lone invalid bytes images as `u"..."`, so its
  image keeps its unit. No 9.5 value needs a prefix.
- Csets use single quotes and escape `'` rather than `"`, as 9.5 does.
- **`image()` is always a literal that rebuilds the value and its
  unit.** A cset images unprefixed, listing every member as 9.5 does,
  whenever that literal rebuilds it and it has at most 256 members. That
  covers every bytes cset, every neutral cset, and so every 9.5 cset. A
  text cset that is larger, or holds lone bytes, images as `u'...'` with
  ranges. Code-point ranges come first, then lone-byte ranges. Runs of 4
  or more consecutive members are written `x-y`, shorter runs are listed,
  and a member `-` is written `\-`. For example, `image(~&ascii)` is
  `u'\u0080-\U0010ffff\x80-\xff'`, and `image(~'a')` is
  `` u'\x00-`b-\U0010ffff\x80-\xff' ``. The images of even the largest
  csets are short, and exact.

### Compatibility cost and open points

All 99 programs in `icon/tests/general` and their expected outputs are
pure ASCII. Of the 1,197 `.icn` files in `icon/tests` and `icon/ipl`, 19
contain non-ASCII bytes (14 of them valid UTF-8); it has not yet been
checked whether those bytes are in literals or only comments.

The four tests that use characters 128–255, traced by hand under the
cset rules above:

- `collate.icn` and `iobig.icn` use only `&cset` (`string`, `reverse`,
  `left`, subscripts, `!&cset`). It is a bytes cset, so both behave as in
  9.5.
- `checkc.icn`: `&cset -- ~&ascii` gives `&ascii` and `image(&cset)` is
  unchanged, both as in 9.5. **Differs:** `&letters ++ ~&ucase` is the
  full text universe (1,112,192 characters), not 256.
- `scan.icn`: `image(~&cset)` is `''` as in 9.5. **Differs:**
  `image(~&ascii)` is `u'\u0080-\U0010ffff\x80-\xff'`, not the 128
  bytes `\x80`–`\xff`.

Both differences are complements of ASCII csets, which is where the new
meaning is intended. They are kept deliberately and will be listed in
the conformance test with this reason.


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
2. **Vertical slice.** Integers, strings, locals and globals, procedures,
   `every`, `to`, `|`, `&`, `if`, `while`, `suspend`, `!` on strings and
   lists, `write`. Translation to IR, lowering to bytecode, and the VM loop.
   The string value carries its unit and ASCII flag from the start
   ("Strings and Unicode").
3. **Co-expressions** in the slice: `create`, `@`, `^`, `&source`,
   `&current`. Confirm the switch cost from the real VM loop.
4. Grow the value types and built-ins, driven by the conformance suite.
5. `gcon build` producing a self-contained executable (the gcon binary
   with the bytecode appended), the counterpart of icont's output.
