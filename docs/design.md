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
  In 9.5 `\u` is not an escape: icont reads `"é"` as the five
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
- **Csets** are sets of code points from 0 to 0x10FFFF, held as a sorted
  range list with a bitmap for 0–255. A literal is read as UTF-8
  (`'αβγ'` has three members). Against a bytes subject, byte b tests code
  point b, so 9.5's byte csets work unchanged. Against a text subject, an
  invalid byte matches a cset exactly when the cset contains U+FFFD. `~c`
  complements over all of Unicode. `&cset` stays the 256 byte values and
  `&uset` (Object Icon's name) is every code point. Go's `unicode` tables
  give category and script csets (names to be settled, e.g. `ucat("L")`,
  `uscript("Greek")`).
- **Fast positions.** A scanning environment caches its cursor
  (`&pos` and the matching byte offset), so scanning functions cost only
  the distance they move. Random subscripts on non-ASCII text use a
  sparse offset index built on first use, one checkpoint every 32–64
  runes, as Object Icon does. ASCII strings need neither.
- Out of scope: normalization, grapheme clusters and display width. The
  unit is the rune, as in Go.

### Compatibility cost and open points

All 99 programs in `icon/tests/general` and their expected outputs are
pure ASCII. Of the 1,197 `.icn` files in `icon/tests` and `icon/ipl`, 19
contain non-ASCII bytes (14 of them valid UTF-8); it has not yet been
checked whether those bytes are in literals or only comments.
Differences from 9.5 appear only through csets and `image()` in the
range 128–255:

- `scan.icn` prints `image(~&cset)`: `''` in 9.5, but every code point
  from 256 up when complement is over Unicode.
- `collate.icn`, `checkc.icn` and `iobig.icn` convert `&cset` to a string
  and print or write it. Their output matches 9.5 only if converting a
  cset with members 128–255 to a string, and `image()` of characters in
  that range, keep 9.5's byte rendering.

To decide when csets are implemented, using those four tests: whether
cset-to-string conversion of 128–255 gives bytes or text, and how
`image()` renders non-ASCII text (9.5's `\xNN` escapes, or `\u` escapes,
or printable characters as Go's `%q` does). Any differences kept
deliberately are listed in the conformance test with their reasons.

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
