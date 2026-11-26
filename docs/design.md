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
and editor extensions, and graphics (see "Graphics: a separate display
server" below).

icontools and icon-sdl remain standalone, for the wider Icon community,
built around the reference implementation. gcon does not depend on them.
It forked the icontools front end (lexer, parser, name resolution,
built-in catalogue, and the icont oracle) at icontools 9cf9615, and the
two evolve independently. Syntax facts are in [syntax.md](syntax.md).

Dependencies: the core (front end, translator, VM, and tools that work on
source) uses only the standard library.

### Decision (2026-11-26): gcon in C; the Go implementation is the reference

**gcon becomes a C program**, with the Go implementation frozen as the
reference it is ported against. "Go showed it can be done. C is how it
*is* done." What carries over from the Go is the design, not the
language: the root layout (`bin`, `lib`, `libexec`, as GOROOT), the
`gcon` command ensemble (run, build, fmt, the language server, ...), and
one repository with one `build.sh` whose only requirement is a C
compiler (and C++ for the display server).

**Why.** gcon-vm, the runtime, is already C (see "the VM in C" below),
and keeping a second runtime in Go forked behaviour at every feature
(`loadfunc`, graphics, `&features`); retiring the Go VM alone would have
left gcon a Go translator in front of a C runtime, two toolchains to
build one program. The front end and tools are small (about 6,200 lines
of Go here, and the formatter and language server about 3,200 in
icontools' fork of the same front end), use little beyond `strings`,
`fmt`, `sort`, `strconv` and `utf8`, and gcon-vm already has the C they
need (buffers, maps, UTF-8 and the Unicode tables, large integers,
iconx-exact real conversion, the program format's decoder, which the
encoder mirrors).

**How: a differential port.** Each part is ported to C and proved
identical to the Go before the next: `gcon build` must produce the same
built program, byte for byte, for every file of the corpus (the
reference tests, the IPL, `icn/`, gcon's test data); `gcon fmt` the same
output; the language server the same replies to recorded sessions; and
the icont oracle tests carry over. Order: lexer and literals, parser,
preprocessor, name resolution, compiler and encoder (`gcon build`
complete), the driver (run, env, the root), the formatter, the language
server. Then the Go goes, all at once. What C makes harder is planned
for: a JSON reader and writer for the language server, `curl` for
fetching the documentation, the generated tables kept as files with
their generators as scripts, the sanitizers throughout and a fuzzer on
the lexer and parser (the language server parses whatever is in the
editor). Windows waits.

**The reference.** The Go implementation is frozen at the commit that
records this decision, tagged `go-reference`: no new features, fixes
only where the port finds that the Go was wrong (and then in both). Its
Go VM is not removed piecemeal; it goes with the rest. Before freezing,
the predefined symbols became the runtime's (gcon-vm's `&features`:
`_DYNAMIC_LOADING`, `_EXTERNAL_VALUES`, `_GRAPHICS`), so the reference
builds what gcon-vm runs; `prepro` is therefore no longer in the Go
VM's conformance list (it passes on gcon-vm, its output iconx's), and
the `keywords` program test leaves the graphics keywords to gcon-vm's
tests. gcon-vm checks its runs against recorded results
(`tests/expected/` there), first recorded from the Go VM.

icontools stays as it is, standalone (above); gcon's formatter and
language server are ported from gcon's own fork.

**Two oracles (2026-11-26, after the parser's port).** Checking the C
only against the Go would make the port C to Go to C, with Icon itself
reached only through the Go. So the authorities are:

1. *Arizona Icon* (icont, iconx, their C sources) for everything Icon
   defines: syntax, semantics, error messages, program results. Where
   it has well-made C for a job, the port starts from that C, not from
   the Go (Icon's sources are public domain; reused code is identified
   as modified).
2. *The Go reference* only for what Icon has no counterpart for: the
   Unicode extensions (cset ranges, string units), the graphics
   protocol, gcon's program format, the tooling (the lossless tree,
   error recovery for an editor, the formatter, the language server),
   and the infrastructure (the distribution layout, GCONROOT, the
   driver's search paths and commands: below).
3. Where the two disagree on something Icon defines, icont wins and the
   Go is fixed.

The differential tests against the Go stay (they are what proves the
Go can go), and the icont oracle tests are ported to run directly on
the C.

The first consequence is in the parser. icont's (yacc's `tparse.c`)
keeps its parse stack on the heap and grows it, so it has no nesting
ceiling; the Go recurses on goroutine stacks, which fail at 1 GB (a
million nested parentheses); a recursive C parser fails at a few
thousand. icont's code cannot be taken whole (it keeps no tokens or
trivia, and its recovery, three `error` rules, is a compiler's, not an
editor's), but its design is: the C parser keeps its own stack on the
heap and does not recurse, and the passes after it walk trees
iteratively, so nothing in the toolchain has a stack ceiling. Its
trees and errors are the Go's for every input the Go can parse.

**Decision (2026-11-26, Sean): the compiler is iterative too.** Here
icont itself has a ceiling: its code generator (`tcode.c`'s `traverse`)
recurses, and crashes at 200,000 nested calls; the Go's recursion
manages that but fails at a million (its 1 GB stack). A recursive C
compiler would fail in the tens of thousands, and a big thread stack
would only raise the ceiling. "Recursion is goto in a fancy frock": the
C compiler keeps its own stack of frames on the heap, as the parser
does, at the price of a harder port. Its output must be the Go's byte
for byte, so each frame makes its side effects (labels, temporaries,
constants, globals, instructions) in exactly the Go's order. (Measuring
this found the Go's compiler quadratic in nesting depth, 17 s for
20,000 nested calls, finding each expression's line by collecting all
its tokens; fixed, with identical output.) The C compiler is done:
byte for byte the Go's over the corpus, linked programs and deep
nesting, and a million levels deep compiles in seconds. Its stacks of
enclosing loops and scans are linked lists (break truncates them, create
empties them, in constant time, by pointing at an earlier node), which
showed the Go's to alias: break's expression, compiled with the stacks
resliced, could append over the enclosing loop or scan, so that a later
next went to the wrong loop, or leaving the break swapped the wrong
scanning environment. Fixed in the Go (copies), with tests
(`internal/run/testdata/breakloop.icn`, `breakscan.icn`).

The preprocessor is the first part ported from icont's code itself
(`src/ipp.c` is icont's `ipp.c`, its output in gcon's form), checked
against `icont -E` (text, positions and errors) as well as the Go. The
port found three places where the Go was not icont's, fixed in both:
an error's offending value is shown as it is, in quotes (the Go quoted
it as Go does, escaping quotes and backslashes); an included file is
looked for on the path as icont's `pathfind` does (each directory, a
"/" added if it lacks one, and the *last component* of the name;
absolute names too; no cleaning of the path), where the Go joined and
cleaned the whole name; and a file that opens but cannot be read ends
at once, where the Go reported "cannot open". A fourth came later, from
the differential test's damaged files: a directive line ends at a NUL
byte, as icont's C strings do (so `$define X "|<NUL>|"` is an
unterminated literal); the Go read on past it. In other lines gcon
keeps a NUL as an ordinary byte, departing from icont on purpose:
icont loses the rest of such a line *and its newline*, joining it to
the next, a defect rather than a definition.

**Scripts are shell, sed and awk (2026-11-26, Sean).** No Python or
Perl in the tree: generators (`src/tests/gentoken.sh`,
`genbuiltin.sh`, `graphics/tools/mkxcolors.sh`) are sh and awk, and a
job that needs a real program (the damaged files of `diff.sh`, which
hold arbitrary bytes) is written in C (`src/tests/variants.c`; in
`vm/tests`, `timeout.c`, `besttime.c` and `damage.c`), built by the
script that uses it.

Name resolution (`src/sema.c`) is the Go's, its diagnostics icont's
(translator `tsym.c`, linker `lglob.c` and `lsym.c`), and is checked
against `icont -u`. Its port found the Go's wording not icont's, fixed
in both: a duplicate record field is `"f": redeclared identifier` (the
Go said "duplicate field name"); a local declared twice is "redeclared
identifier" only if both are the same kind (parameter, local, static),
and "inconsistent redeclaration" otherwise (icont's `install`); an
unknown keyword is `"x": invalid keyword` (the Go wrote `&x`); and an
inconsistent redeclaration says no more than icont's text. The other
declaration it conflicts with, which the Go put in the message ("also
a record in a.icn"), is now the diagnostic's `Related` symbol, for the
language server to give as related information. Positions stay gcon's
(a line for the linker's errors too, which icont gives only the file),
and undeclared identifiers are flagged in every procedure, where icont
-u reports only those its linker keeps (an editor wants them all).

**Decision (2026-11-26): the driver and infrastructure are gcon's.**
What a file *means* is icont's to say; how the toolchain is laid out
and finds things is not. gcon's distribution layout, the GCONROOT
protocol (Go's GOROOT model), and the search order built on them (the
current directory, LPATH or IPATH, then the root) stay as the Go
reference has them, and the Go is their oracle. So the C does not take
icont's extra search of the source file's own directory (`tunix.c`
prepends it to LPATH), nor its fallback to a `lib` directory beside
icont. Within one directory, how a name is turned into a path is still
icont's (`pathfind`, above).

### Graphics: a separate display server

**Decision (2026-11-25): graphics runs in a separate display server
process, X11 / Plan 9 style.** gconx and the server talk a protocol over
a socket. This supersedes the earlier plan (same day) to link icon-sdl's
backend into gcon through cgo; the choice of icon-sdl's backend over a Go
toolkit such as Fyne stands, but it becomes the server.

**Division of labour.** gconx owns everything Icon's graphics model
demands of the language: the event queue as an Icon list (`Pending()`),
`Event()` blocking and polling, `&x`, `&y`, `&interval` and the other
event keywords, `WDelay`, window and context values. The server owns the
display: it creates windows, keeps the canvases, draws, and feeds raw
events back. It can be written in any language and run on its own.

**Why.**

- gcon stays pure Go: no cgo, no C++ toolchain, no build tag,
  cross-compilation intact, and `go install` of gcon never needs C.
- The macOS rule that SDL owns the main thread is the server's concern
  alone; gconx needs no `LockOSThread`.
- A server crash cannot corrupt the interpreter's state.
- Precedent is close: plan9port's `devdraw` is a helper process that
  libdraw (and Go's `9fans.net/go/draw`) drives over a pipe, adopted
  partly to get Cocoa's main thread out of the client.

**Canvases live in the server** (as in X11 and Plan 9's `/dev/draw`),
not in the client (as in Wayland). A client-side canvas would make
`Pixel`, `CopyArea` and `drawop=reverse` local, but would need a
rasteriser in gconx: Blend2D through cgo again, or a pure-Go one built
from scratch. With server-side canvases the reference server is
icon-sdl's backend almost unchanged (branch `sdl-graphics`, design in
`icon-sdl/sdl/design.md`), wrapped in a small C `main` that reads
requests:

- **Canvas layer** (`src/sdl/canvas.c`): each window is an RGBA image at
  the display's backing scale, drawn with Blend2D, holding the graphics
  context (colours, line style, drawop, fonts, clipping, patterns) and
  the pixel-level operations Icon defines (`drawop=reverse` as XOR,
  `CopyArea`, `Pixel`, images). Behaviour has been compared against the
  X11 backend, which is the reference.
- **Platform layer** (`src/sdl/plat.c`): SDL3 windows, presentation,
  events, cursors; font discovery by CoreText (macOS) and fontconfig
  (Linux).
- **Dependencies** are vendored source tarballs pinned by SHA-256
  (`deps/deps.lock`: SDL3, Blend2D, AsmJit), and builds never use the
  network. The server keeps that discipline, with its own build.
- **Threads.** The server has no interpreter, so SDL keeps the main
  thread and requests are read from the socket by another thread (or
  woken into the SDL loop); icon-sdl's main-thread request mechanism
  carries over. The new icon-sdl files are MIT, by Sean Jensen, so they
  can be used as they are.

**Protocol rules.**

- **Shaped for latency.** Icon draws in many small synchronous calls.
  Drawing requests are fire-and-forget into a buffer, flushed when gconx
  blocks (`Event`, `WDelay`, `delay`), flushes (`WFlush`, `WSync`), needs
  a reply, or exits. Round trips are kept to calls that need an answer:
  `Pixel` (one request per rectangle), opening a window or font, geometry
  queries. A local socket round trip is some tens of microseconds,
  comparable to the ~35 µs icon-sdl already pays per main-thread request.
- **Answer locally where the semantics allow.** Colour names (Icon's
  colour grammar is fixed) and attribute bookkeeping are done in Go; font
  metrics and glyph advances come back once when a font is opened, so
  `TextWidth` and `WAttrib("ascent")` need no round trip.
  `ReadImage`/`WriteImage` use Go's `image/gif` and `image/png`, sending
  pixels rather than file names, so the server never touches the
  program's files.
- **No asynchronous errors** (X11's trap). Anything that can fail in Icon
  terms (bad colour, font not found, window open failing) is either
  decided in gconx or is a request with a reply. Pipelined drawing
  requests cannot fail; a server error on one is a protocol error that
  ends the program.
- **Objects by id:** windows (canvases) and graphics contexts, so
  `Clone` and `Couple` map onto X11's drawable / GC split.
- **Version handshake** first on every connection.
- **Startup.** gconx starts the server lazily on the first
  `open(..., "g")`, as a child on a socketpair, found next to the gcon
  binary or named by an environment variable (which could instead name a
  socket to connect to, giving remote display).

**Testing.** A headless server in Go (drawing into `image.RGBA`, or just
recording requests) gives golden tests for the graphics functions with
no display; protocol transcripts are diffable.

**Costs accepted.** A protocol to design, document and version; two
programs to distribute and to find at run time; bulk pixel traffic for
images (shared memory is a later optimisation if it ever matters).

**The server** is `gcon-graphics`, in its own repository, forked from
icon-sdl's backend and vendored deps (2026-11-25). Its design notes
cover the server side and the protocol.

**Wire format:** modelled on Plan 9's draw protocol (draw(3)): compact
binary messages, client-chosen integer ids, few replies.

**Open question:** how the Go headless server and gcon-graphics are kept
in step (a shared conformance suite over the protocol).

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

### Decision (2026-11-25): values as tagged structs

A Value was a Go interface (`any`). Storing an integer or a real in an
interface allocates (Go preboxes only 0 to 255), and so does a variable
such as `L[i]`, a struct; profiles of the VM showed boxing and the
garbage collection it causes as the largest cost after the string
functions were fixed. Working around it (preboxed small integers,
inline integer paths) recovered some of the gap with iconx, not the
gap, so a Value becomes a tagged struct:

```go
type Value struct {
	k Kind           // the type, or the kind of variable
	n uint64         // an integer, a real's bits, or an index
	p unsafe.Pointer // the heap object, or nil
}
```

Integers, reals and `&null` are held in the struct; strings, csets,
large integers and structures by pointer; a variable is a kind with a
pointer and an index (a list element is the list and the index, a
field the record and the field's number; a local or global, a pointer
to its slot), so making one allocates nothing. The kind 0 is "no
value" (Go's nil interface in the old representation), distinct from
`&null`. `p` always holds a real pointer or nil, so the garbage
collector sees the struct as it would any pointer. Values are 24 bytes
instead of 16, so lists and tables take half as much memory again.

**Evidence** (`experiments/valuerep`): two interpreters with gcon's
dispatch loop, frames and instruction layout, differing only in the
representation, running the same loops. The struct is 3.2 times as
fast in each, with no allocation: an integer loop `x := i + 407` 12.3
ns to 3.9, a subscript `v := L[3]` 15.8 to 4.9, a real loop `r := r +
1.5` 12.4 to 4.0 (Apple M-series). iconx takes 7 ns for `4 + 7`,
including its loop.

**Migration** (done, 2026-11-25). The VM's code works through
constructors and accessors (`v.Kind()`, `v.Int()`, `IntVal(i)`,
`v.AsList()`, ...) in place of Go type switches and assertions. It was
done in two mechanical steps, each checked by the full test suite, the
conformance tests and the differential test against iconx:

1. `V(x)` and `v.Any()` bridged the new representation and the old
   concrete types, and a throwaway codemod using `go/types` rewrote the
   791 places the type checker flagged; 16 more by hand.
2. A second codemod turned `V(x)` into the constructor for `x`'s static
   type, comma-ok assertions into `AsT` methods and type switches into
   switches on the kind; the hot paths (dispatch loop, calls,
   generators, variables, conversions) were then made native by hand.
   `V` and `Any` remain for code off the hot paths.

Along the way: a generator in a Value is its pointer and a number
naming its type (`gen.go`; a closure generator by its closure pointer,
which a test checks), so holding one allocates nothing; a saved
scanning environment is held in the Value; table keys hold structures
by pointer, and deleted table entries are reclaimed in place unless a
traversal shares the arrays (copy-on-write).

**Result** (2026-11-25, Apple M-series). The empty `every` loop takes 4
ns against iconx's 7; integer arithmetic and comparison are faster
than iconx's; `L[i]` 19 ns, without allocation. `micro.icn`'s geometric
mean is 2.35 times iconx's time: the core operations gained, and what
remains is in built-in functions (most scanning and string functions
take four to six times iconx's time) and in strings, each of which is
a `*String` allocated on the heap where iconx's are a pointer and a
length into its string region. Of the benchmark programs, `rsg` runs in
a third of iconx's time and `queens` level; `deal`, `concord` and
`ipxref` (string and table work) are slower. Output is identical, and
the differential test shows the same five known differences.

**Strings in Values** (2026-11-25). `String` is a value type, and a
string Value holds a pointer to its bytes with the length (56 bits),
unit and ASCII flag in `n`, so making a string Value or a substring
allocates nothing, as in iconx. `NewString` finds whether a string is
ASCII as it is made; a substring of ASCII is ASCII; otherwise the flag
is found when needed. A saved scanning environment packs the subject's
length (32 bits) and the position (29 bits) beside the unit and flag,
falling back to an allocated record (`KScanEnvBox`) beyond those.
Generators take their arguments from the scratch buffer, since they
take what they need when called (reversible assignment, which keeps its
variables, copies them; the poison-argument run found it), and how a
builtin's arguments are adjusted is worked out once rather than looked
up by name at each call. `micro.icn`'s geometric mean went from 2.35 to
1.97 times iconx's time (3.51 before the day's work); 12 operations are
faster than iconx's, 8 within 1.5 times, 64 within three. What remains
is mostly the number of instructions an operation takes and the cost
of a call to a built-in function.

**Built-in calls and structures** (2026-11-25). Unary `*`, `+`, `-` and
`===`/`~===` have inline paths like integer arithmetic's; `tab`, `move`
and `=s`, which produce one value and restore `&pos` when resumed, keep
the old position in the handle instead of a generator object
(`Builtin.Restore`); `upto` and `find` are struct generators (one
allocation, not a closure and its captured offset); `push` reuses the
room before a list's first element, so a run of pushes and pops does
not allocate (`TestListDeque` checks the list against a slice over
random sequences); a frame's locals and temporaries are one
allocation; a cset caches its complement; `copy` of a table or set
clones its index without hashing again. `micro.icn`'s geometric mean is
now 1.61 times iconx's time (3.51 at the start of the day): 14
operations faster than iconx's, 29 within 1.5 times, 55 within three,
14 beyond. The rest is mostly a procedure call (about three times
iconx's), `suspend`, conversion of strings to numbers, and the
instruction count of compiled expressions.

### Decision (2026-11-25): the VM in C (gcon-vm)

A stock-take after a day of performance work, which took the Go VM from
3.51 to 1.61 times iconx's time over `micro.icn`.

- **Wins.** Co-expressions are cheap by design: a non-recursive VM with
  frames that are not on the C stack makes activation a frame switch
  (21 ns, against 2611 for iconx on macOS, which switches threads). The
  display server keeps graphics out of the interpreter's process and
  language. The toolchain (front end, `build` and `dis`, the
  built-program format, GCONROOT, the conformance, program and
  differential tests) does not depend on the VM's language.
- **Loss.** Go is the wrong language for a VM meant to match iconx. It
  reached within 1.6 times, and beat iconx on loops and arithmetic, but
  only by working against the runtime (`unsafe` for tagged values,
  generators dispatched by a type number, closures held by pointer);
  and it keeps costs C does not have: a write barrier on every store of
  a value into the heap while the collector marks, frames on the heap,
  bounds checks, switch dispatch, allocation through the collector.
- **Neutral.** UTF-8 processing is straightforward with positions as
  bytes; Unicode's semantics (two units, csets with code-point ranges,
  `image()`, mixed operands) bring complexity of their own, whatever the
  language.

So gcon's VM moves to C, in its own repository, **gcon-vm**, beside gcon
and gcon-graphics, keeping C builds out of this one: gcon remains the Go
toolchain (translation, `build`, `fmt`, the language server) and hands
bytecode, in the built-program format, to gcon-vm. gcon-vm ports this
VM's design and semantics rather than extending iconx, whose
thread-switched co-expressions and 8-bit strings and csets run through
its whole runtime. A spike (gcon-vm's `experiments/spike`, beside this
repository's `experiments/valuerep`) measured C at about twice Go's
speed for the same design: `x := i + 407` 2.1 ns against 3.7, `L[3]`
2.7 against 5.2, a call with frames on a stack 3.5 against 7.4. The Go
VM stays as the reference implementation, and as a second oracle in
testing, until gcon-vm passes the tests; gcon-vm's `docs/design.md`
has the plan.

### Decision (2026-11-26): gcon run uses gcon-vm

gcon-vm runs gcon's built programs with the same results as this VM
(gcon's program tests, the reference tests, 260 IPL programs), at about
half its time, and co-expressions at a hundredth of iconx's. So `gcon
run` hands programs to it when it is installed (`internal/run`,
`gconvm.go`):

- **Which VM.** `$GCONVM` if set: a gcon-vm, or `go` for this VM; else
  `libexec/gcon-vm` in the root, if executable; else this VM. `gcon env`
  shows the choice (`GCONVM`, `GCONVMFROM`).
- **A built program** is run as it is: gcon `exec`s gcon-vm on the file,
  so gcon's process becomes gcon-vm's, and the exit status, signals and
  standard streams are simply its own; `&progname` is the name the
  program was invoked by, as before.
- **A built program's header** runs gcon-vm itself where it can, so as
  not to start gcon: `$GCONVM` if it names one; else, unless `$GCON` or a
  gcon beside the program is there to decide, the gcon-vm of the root of
  the gcon that built it, recorded in the header (`GCVM`) as the gcon's
  path is; else `gcon run` as before (and so `GCONVM=go` goes through
  gcon to this VM). The header also finds the gcon beside the program by
  parameter expansion rather than `sed`, so it starts no command but the
  VM. Hello world now starts in 6.6 ms (gcon-vm itself 4.2, the rest
  sh), against 8.0 through gcon and 11.2 for icont's output on iconx.
  `TestHeaderChoice` checks the order, with stand-ins for gcon and
  gcon-vm.
- **Source** is translated as before, built to a temporary file deleted
  at once (gcon-vm reads it as `/dev/fd/N`, inherited across the
  `exec`, rewound first since opening `/dev/fd/N` shares the offset on
  macOS), and run with `-progname` set to the name the program has from
  source (`vm.ProgName`). Translation errors are reported by gcon as
  before. Nothing is left on disk.
- **Unix only.** gcon-vm is POSIX C; elsewhere gcon uses this VM.
- **This VM stays**, in-process: the tests run it (it is the reference),
  and `gcon run` falls back to it. `TestExecVM` runs every program test
  from source through the hand-over itself, when `GCONVM` names a
  gcon-vm (it is skipped otherwise): its test binary re-runs itself as
  a child that calls `ExecVM`, since `exec` replaces the process.

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
  take. gcon uses Go strings without the annotations: positions are
  bytes, as in Go, so `*s` and `s[i]` need none.
- **Object Icon** (objecticon.sourceforge.net, "Unicode") adds a `ucs`
  type: a UTF-8 string with an offset index for random access, written
  `u"..."`. Its csets range over 0 to 0x10FFFF, and it adds `&uset`,
  `uchar` and `ucs()`. Unicode is opt-in there (a `u` prefix, plus
  `$encoding UTF-8`), and mixing a `ucs` with an invalid string is a
  runtime error.

### Decision: one representation, two units

- A string value is a Go `string` plus a **unit**, text or bytes. The unit
  says how the string's characters are read; it never changes positions
  or lengths, which are bytes (see "Positions are bytes" below).
- **Malformed UTF-8 is handled as Go handles it.** A text string may hold
  any bytes, and they are never rewritten. Reading it as characters
  follows `utf8.DecodeRuneInString`: each invalid byte is a one-byte
  character whose code point is U+FFFD. So `len(s)` is
  `utf8.RuneCountInString`, and `!s` yields exactly what a Go `range`
  loop does. Since every character is a substring of the original bytes,
  `s` is always the concatenation of `!s`, and any data round-trips
  unchanged. This is also what makes byte positions safe: a slice through
  a character yields lone bytes, which rejoin to the character.
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
- **I/O.** Files are text by default; mode `"u"` makes a file bytes.
  (In 9.5, `"u"` means "untranslated", i.e. binary, and is ignored on
  Unix; `"b"` already means "both", read and write, so it cannot mean
  bytes.) Reading does no validation or rewriting; it only sets the
  unit. In text mode `reads(f, n)` reads n characters and never splits
  a sequence; `read` strips `"\n"` or `"\r\n"`, as iconx does.
- **Csets** have no unit; their characters are ASCII, bytes and code
  points (see "Csets" below).
- Out of scope: normalization, grapheme clusters and display width. The
  unit is the rune, as in Go.
- **UTF-8 is the only text encoding.** As in Go, text means UTF-8 and
  anything else is binary. gcon does not transcode or guess encodings:
  a Latin-1 (or other) file read in text mode keeps its bytes, and its
  non-UTF-8 bytes read as U+FFFD; to work with it, read it as bytes.
  Latin-1 bytes in source literals make those literals bytes, which is
  what preserves 9.5's behaviour for them, not support for Latin-1 text.

### Positions are bytes

**Decision (revised 2026-11-25).** As in Go, a position, subscript or
length is a byte offset; an operation that reads characters decodes them
at that offset. (The first design counted every position in the string's
unit, characters in text.)

| Bytes: positions, subscripts, lengths | Characters: operations that read them |
|---|---|
| `*s` (Go's `len`) | `len(s)` (Go's `utf8.RuneCountInString`) |
| `s[i]`, `s[i:j]`, `s[i+:n]` | `!s` (Go's `range`) |
| `&pos`, `tab(i)`, `pos(i)` | `move(n)`: n characters, forward or back |
| positions produced by `upto`, `many`, `any`, `find`, `match`, `bal` | the tests those functions make, and their steps |
| the `i, j` range arguments of the string analysis functions | `reads(f, n)` |
| | `map`, `reverse`, `trim`, `cset(s)`, `ord`, `image` |
| | widths in `left`, `right`, `center`; `detab`/`entab` columns |

- **Why.** It is exactly 9.5, whose positions are byte positions. The unit
  no longer affects positions: `runes(s)` and `bytes(s)` have the same
  length and positions, and `*(a || b) = *a + *b` always. Slicing anywhere
  is safe, because of the malformed-UTF-8 rule above. Positions produced
  by the scanning functions fall on character boundaries whenever the
  subject is valid UTF-8, since those functions step over whole
  characters, so `tab(upto(c))` and `tab(many(c))` never split one.
  Indexing is constant-time, with no cursor or offset index.
- **Where it bites, as in Go.** `"é"[1]` is the lone byte `"Ã"` and
  `*"é"` is 2. `tab(&pos + 1)` can split a character where `move(1)`
  cannot: `move` is Icon's "next character", the scanning counterpart of
  a `range` step. `match`, `find` and `=s` compare bytes, as Go's
  `strings.HasPrefix` and `strings.Index` do.
- **Padding widths are characters**, as Go's `fmt` widths count runes:
  the least surprising choice for lining up text, short of display width
  (East Asian wide characters, combining marks), which stays out of scope.
- **`len`** is gcon's built-in for the character count. The corpus uses
  `len` 135 times, always declared (or as a record field), so the
  built-in changes no program. It is a deliberate false friend: Go's
  `len` of a string counts bytes, which here is `*s`. The name was kept
  for its shape, on the understanding that it is documented clearly; it
  is no more of a pitfall than Icon's 1-based positions.

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

## Linking

### Why icont has .u1 and .u2

icont translates each file on its own into two ucode files: `.u2` holds
the file's global facts (declared globals, procedure and record names,
`link` and `invocable` declarations), `.u1` its code. Translating a
file alone, icont cannot tell whether an undeclared identifier is a
global or a local, so `.u1` leaves such names unresolved. The linker
(`link.c`) therefore works in phases: read every `.u2`, following `link`
declarations transitively, to build the global table; optionally scan
the `.u1` files to drop procedures nothing reaches (`scanrefs`); then
read the `.u1` files, resolve the names, and generate code. Keeping the
global facts in their own small file let phase one avoid reading code
and let each `.u1` be read as a forward-only stream, which mattered on
the machines Icon was written for. The phases need two sections, not
two files: a single file with its global section first serves as well
(Unicon has a single `.u`, as we understand it).

### Decision (2026-11-25): link from source, no build files

gcon has no ucode (its bytecode is its own) and translates quickly, so
`link` works on source: `link foo` finds `foo.icn`, searching the
current directory and then the directories of `IPATH` (separated by
spaces or colons), as icont's `pathfind` searches for `foo.u1`; `link
"path"` names a file. Linked files are found transitively, each once,
and the whole program is resolved and compiled together, which is
icont's phases one and three done in memory. Nothing is written beside
the sources. If translating large libraries ever becomes slow, compiled
units can be cached in a per-user cache directory keyed by a hash of
their contents, one file per unit with its global section first; that
is an optimisation for later, not a format to design now. icont's rules
carry over: inconsistent redeclarations across files are errors, and
procedures nothing reaches and nothing declares invocable are dropped.
The Icon Program Library is used from its source, which gcon finds in
its root after `IPATH` (see "Decision: a gcon root, as GOROOT").

Implemented in `run.TranslateLinked`: files are read breadth first from
the main file, each once (by absolute path), so cycles are harmless. A
candidate that declares `procedure main` is passed over and the search
goes on: it cannot be linked into a program (it would redeclare main),
and passing over it is what icont does in effect, since a program's
source has no `.u1` unless someone translated it as a library. Without
this, `link lists` from an IPL file run in icon/tests/general finds the
test program `lists.icn` there. a
linked file is known in the program by the name it was linked by plus
".icn" (`&file`, tracebacks and trace lines say "printf.icn"), as icont
knows a library file by the name it was translated under. An unfound
file is an error at its `link` ("cannot resolve reference to file
'foo.icn'"; icont names 'foo.u1'). A redeclaration across files is
reported at the later declaration with the earlier one named, where
icont's linker gives only the file. Program tests link files from
`internal/run/testdata/lib`, which `expect.sh` translates with `icont
-c` for iconx; the conformance test links the IPL from `ipl/procs`.

### Decision (2026-11-25): the built program is an icont-style script

`gcon build` writes one file, the counterpart of icont's icode
executable, and that is its only artefact: no self-contained binary.
Programs are meant to be invocable files in an Icon/gcon environment,
not binaries to distribute, and people coming from Icon already have
an installation and expect this. (A self-contained binary, gcon with
the program appended, was considered: one file and no dependence on an
installed gcon, but a few MB per program, built per platform, and on
macOS appending to the binary fails strict code-signing validation.)

The file follows icont's: a `#!/bin/sh` header that finds gcon and
execs it on the file with the arguments, then a marker line, then the
program. icont's header tries `$ICONX`, then iconx beside the program,
then the iconx it was built with, then `iconx` on `PATH`; gcon's tries
`$GCON`, gcon beside the program, the gcon that built it, then `gcon`
on `PATH`. `&progname` is the file's name as invoked (`$0`), as in
iconx.

What follows the marker is the program as linked bytecode, as in
icont: a serialisation of the whole compiled `vm.Program` (code,
constants, globals, records, procedures), the counterpart of icode,
written once and only when asked for. Built-in functions and operators
are recorded by name, not as Go function values. The format carries a
version; gcon refuses a file whose version it does not read, saying to
rebuild it, as a newer iconx refuses old icode. (Bundling the linked
sources instead, translated at each run, was considered and set aside.)

Reconsidered the same day, with measurements: translating at each run
is cheap. For `phwbi` in `icn/` (seven local libraries and the IPL's
graphics library, 1167 globals), translation takes 25 ms and `gcon run`
to its usage message 32 ms, against 8 ms for iconx running the icont-
built program and 92 ms for icont to build it; a small program
translates in 2 ms. So running from source (an executable `.icn` with a
`#!/usr/bin/env gcon` first line, which is an Icon comment) would be
fast enough. The snapshot was kept for what it isolates: a built
program depends only on gcon, not on the application's source tree and
the library sources (`IPATH`, `LPATH`) being present and unchanged at
run time. Defining the serialisation is a one-off cost.

### The built program's format

Defined in `internal/vm/encode.go`; `gcon dis` lists a built program (or
source) in full. The file is the shell header, the marker line
`[gcon program follows]`, then the encoded program:

- **Magic** `\x89GCN\r\n\x1a\n` (PNG's device: a non-ASCII first
  byte and line endings that a text-mode copy would mangle), then the
  **format version**, a varint. gcon reads exactly one version; any
  other is refused with "program built by a newer (older) gcon ...:
  rebuild it", as iconx refuses icode of another version. A file is
  recognised as built when it starts `#!` and a line holding the
  marker is followed by the magic, so `gcon run` takes either source
  or a built program, and the header simply runs `gcon run "$0" "$@"`.
- **Sections**, each a four-byte tag, a varint length and that many
  bytes. As in PNG, a tag whose first letter is upper case is
  required: a gcon that does not know it refuses the file ("built by
  a newer gcon?"). One with a lower-case first letter may be skipped,
  so information can be added later without invalidating built
  programs. The required sections come in a fixed order:
  `Bltn` (builtins), `Recd` (record types), `Proc` (procedures),
  `Cnst` (constants), `Glob` (globals with their names and initial
  values, then main and the invocable set). The optional `info`
  section holds the build information: gcon's version and the
  preprocessor's predefined symbols, which are decided when the
  program is built (a program built without `_GRAPHICS` keeps its
  non-graphics branches). The last section, `End `, holds the CRC-32
  of everything before it.
- **Builtins by reference.** A program holds no Go function values:
  each builtin is a (table, name) pair: an `Ops` key (`u-` for unary
  minus, `&pos`), a `Functions` name, an assignment marker, `!`, a
  field operator `.name`, or a stub for an unimplemented function.
  Decoding a pair gives the very same `*Builtin` (the VM compares
  some by identity), so `FieldOp`, `Unsupported` and `AsgnOp` are
  memoised, one per name. An unknown name is refused at load ("built
  by a newer gcon?"), not left to fail when called.
- **Values** are tagged: null, integer, large integer (sign and
  magnitude), real (its IEEE bits, so exact), string (unit and
  bytes), cset (its byte bitmap and code-point ranges), and builtin,
  procedure or record type by index in their sections, so that each
  stays one shared object.
- **Instructions**: the opcode, a byte of flags saying which fields
  are nonzero, the line as a difference from the previous
  instruction's, then the present fields (operands as
  `index<<3 | kind`). Writing only nonzero fields, rather than a
  fixed shape per opcode, keeps the format independent of which
  fields each instruction uses.

**What changes the version.** The opcode numbers in `instr.go` are now
explicit and are part of the format: an opcode is never renumbered or
reused. Adding one, changing what an instruction does with its fields,
or changing the encoding changes `FormatVersion`. So does a change in
what the compiler decides at build time that a built program would
keep: for example, the graphics keywords (`&x`, `&col`, ...) now compile
to failure, so when graphics arrives the version should change and old
built programs be rebuilt. New builtins do not change it: names are
resolved at load.

**Checking.** Decoding checks every count, index and label, the operand
kinds the VM relies on (`vm.Validate`), and the checksum, so a
truncated, damaged or mismatched file is reported, not run. This guards
against accidents, not malice: a built program is a program, and can do
what it likes anyway. Tests: every test program and 621 programs of the
reference tree round-trip (the decoded program lists identically, its
builtins are the same values, and re-encoding gives the same bytes);
every program test runs again from its built file; every truncation
and every changed byte of a sample is reported; unknown optional
sections are skipped and unknown required ones refused.

Measured on `phwbi` (see above): the built file is 342 KB (icont's
icode is 717 KB) and runs to its usage message in 5 to 10 ms, against
30 ms from source and 8 ms for iconx.

`gcon build file.icn` writes `file` in the current directory, as icont
does (`-o` names it), executable where readable. `gcon dis file` lists
constants, globals and each procedure's instructions, with builtins by
reference (`[op u-]`) and every field shown.

### Decision (2026-11-25): a gcon root, as GOROOT

The IPL ships with gcon as source files in a root directory, as Go's
standard library ships in GOROOT, not embedded in the binary: its
sources stay there to read, learn from and copy, and the language
server can link to them. This is icont's arrangement made explicit:
icont always appends `<bin>/../lib` to `IPATH` and `LPATH`
(`tunix.c`, `libpath`), where Icon's installation puts the translated
library.

```
$GCONROOT/
  bin/gcon
  libexec/gcon-graphics     the display server, when there is one
  libexec/gcon-vm           the VM in C, when installed (gcon-vm's tools/install.sh)
  lib/ipl/procs, gprocs     the IPL's procedures (link)
  lib/ipl/incl, gincl       its include files ($include)
  lib/ipl/UPSTREAM          the upstream commit and each file's SHA-256
```

`lib/ipl` keeps the IPL's own directory names, and leaves room for a
library of gcon's own beside it.

**Search.** `link` searches the current directory, then `IPATH`, then
`lib/ipl/procs` and `lib/ipl/gprocs`; `$include` searches the current
directory, then `LPATH`, then `lib/ipl/incl` and `lib/ipl/gincl`. To
change a library file, copy it to a directory on `IPATH` (or `LPATH`),
where it comes first. A built program is a snapshot, so the root
matters only when building, never when running.

**Finding the root** (`internal/root`), as Go finds GOROOT:

1. `$GCONROOT`, used as it is.
2. The directory above the executable, after following symbolic links,
   if it holds `lib/ipl/procs`: an unpacked release, a symbolic link
   such as `/usr/local/bin/gcon`, or a repository checkout, which is
   itself a root once gcon is built into it (`go build -o bin/gcon
   ./cmd/gcon`), as Go's source tree builds `bin/go`.
3. A default set at link time, for packagers who put the binary and
   the library apart: `-ldflags "-X
   github.com/phonologus/gcon/internal/root.defaultRoot=DIR"`.
4. The module cache, for `go install
   github.com/phonologus/gcon/cmd/gcon@VERSION`: the go command has
   downloaded the whole module, `lib` included, to
   `$GOMODCACHE/github.com/phonologus/gcon@VERSION` (the cache found as
   the go command finds it: `GOMODCACHE`, else `GOPATH`'s first
   directory plus `/pkg/mod`, else `$HOME/go/pkg/mod`; upper-case letters
   escaped as `!` and lower case). It is read-only but readable, and
   matches the binary's version exactly. Only a released version is
   looked for there, not a development build, and `go clean -modcache`
   removes it.

`gcon env` prints the root, how it was found, `IPATH`, `LPATH`, the
resulting search paths and the version, in a form sh can read. A
failed `link` with no root found adds a line pointing to it.

**Distribution** is then a tarball of the root
(`gcon-VERSION-OS-ARCH.tar.gz`), unpacked anywhere with its `bin` on
`PATH`, as Go's is; `go install` serves without graphics.

**The library's copy.** `tools/ipl/update.sh` copies `procs`, `gprocs`,
`incl` and `gincl` from the local clone of the reference
implementation (never the network) and writes `UPSTREAM`; a test
checks that the files are exactly those it lists. The reference
implementation declares its material public domain, but
`gprocs/cardbits.icn` carries copyright and permission notices for its
card bitmaps (from the XView "Spider" game: Sun Microsystems, Network
Computing Devices, Donald Woods, David Lemke, Heather Rose), each
asking to appear in supporting documentation. That is probably an
oversight upstream; as a precaution the file is distributed unchanged
and `NOTICE`, at the top of the tree, reproduces the notices verbatim
(a test checks that it does). A release tarball of the root includes
`NOTICE`, `LICENSE` and `lib/ipl/README`.

## The preprocessor

`internal/ipp` is icont's preprocessor (`src/common/ipp.c`), ported
step for step: `$define`, `$undef`, `$ifdef`, `$ifndef`, `$else`,
`$endif`, `$include`, `$line`, `$error`, and `#line` at a line's start;
substitution of whole identifiers outside literals, numbers and
comments, rescanned, never within its own definition; icont's error
messages. Every file is preprocessed, each on its own from the
predefined symbols, as icont translates each file on its own. The
predefined symbols are `_V9` and one for each of gcon's `&features`
(`_UNIX`, and `_MACINTOSH` on macOS, as iconx's macOS build defines it;
`_ASCII`, `_CO_EXPRESSIONS`, `_KEYBOARD_FUNCTIONS`, `_LARGE_INTEGERS`,
`_PIPES`, `_SYSTEM_FUNCTION`), each "1". `$include` searches the
current directory, then `LPATH`, as icont's `pathfind`.

Positions survive `$include` because the output has exactly one line for
each line read (directive and skipped lines become empty, and a
definition is a single line), and a map records each output line's file
and line. Name resolution and the compiler take every position from the
map (`sema.File.Locate`); an instruction carries a file index as well as
its line (`Instr.File`, `Proc.FileAt`), so code included in the middle
of a procedure reports its own file in `&file`, traces and tracebacks.
`$line n` makes the directive line n, so the next is n+1, as icont's
`#line` markers do. Once a file is preprocessed, none of its identifiers
is a preprocessor symbol for name resolution (`sema.File.Preprocessed`).

Verified: `ipp`'s `TestOracle` compares the output with `icont -E` on
every corpus file icont can preprocess (1188 of 1203), ignoring blank
lines and icont's `#line` markers, adding the two predefined symbols of
the reference icont that gcon lacks (dynamic loading, external values);
`testdata/preproc.icn` checks expansion, conditionals, `$include` within
a procedure, `$line`, the predefined symbols and digraphs against iconx.

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
- Reals format as iconx's `rtos` does: C's `%.10g`, with `.0` added when
  there is no decimal point or exponent, and `inf.0` for infinities. Go's
  `strconv.FormatFloat(x, 'g', 10, 64)` is `%.10g` exactly. Arithmetic,
  conversions and `^` follow iconx's `oarith.r` (`ripow` is reproduced
  step for step, so results agree to the last bit); `to` truncates real
  operands to integers, as iconx does. Errors 204, 205 and 206 are raised
  where iconx raises them.
- String functions (`internal/vm/strfuncs.go`) follow iconx's `fstr.r`
  step for step (the order in which `left`, `right` and `center` lay
  down copies of the pad, and `center`'s parity adjustment), working in
  characters of the strings' units. `detab` and `entab` count a
  printable code point as one column in text.
- Files (`internal/vm/file.go`): `open` (modes `r w a b c u p`, pipes
  through `/bin/sh`), `close` (a pipe's close returns its command's exit
  status), `read`, `reads`, `write` and `writes` with file arguments,
  `stop`, `flush`, `seek`, `where`, `remove`, `rename`, `getenv`,
  `system`, `!f`, and `&input`, `&output`, `&errout`. As in iconx,
  standard output is flushed before anything is written to `&errout`, so
  the two stay in order when they go to one file. `system` returns C's
  wait status (exit code times 256), but a pipe's `close` returns the
  exit code itself: iconx returns `(pclose(fp) >> 8) & 0377` (fsys.r).
  Commands run as C's `system` and `popen` run them: `/bin/sh` with
  `argv[0]` "sh" (so the shell's messages begin "sh:"), inheriting
  gcon's standard streams; a command read through a pipe reads gcon's
  standard input. gcon passes its streams to a command as the
  operating-system files they are, so a command's output never depends
  on gcon relaying it; only when gcon was given other streams (as in
  tests) does it relay them, and a pipe command then gets empty input,
  since a reader cannot be shared. Open files are flushed when the
  program ends, as C's `exit` does, and pipes written to are closed so
  their commands see the end of their input and finish; gcon waits for
  them only if it is relaying their output (iconx does not wait).
- The evaluation stack (`push` in `internal/vm/vm.go`). gcon's frames
  are on the heap, so recursion is bounded only by memory; runaway
  recursion stops, as in iconx, with run-time error 301, which `&error`
  never converts (though `runerr(301)` is converted, as any error is).
  Each co-expression counts what its active calls would take on iconx's
  stack: nine descriptors a frame plus one per parameter and local,
  against a capacity of 500000 descriptors, or `MSTKSIZE` words at two
  to a descriptor. Measured against iconx, this gives its overflow depth
  to within two calls for simple recursion (55554 calls of a procedure
  with no parameters or locals, 45454 with one of each, 29412 with three
  and five). iconx's stack also holds expression temporaries, which the
  model leaves out, so where those are deep gcon goes deeper (the IPL's
  `lisp` overflows at 45455 calls, iconx at 13158). A suspended frame
  is not counted, as it is not in `&level`.
- Tables and sets (`internal/vm/structs.go`) are insertion-ordered maps
  keyed by `===`: numbers, strings and csets by value (a string's unit
  counts only if it is non-ASCII), structures by identity.
- **Decision: iteration order is randomised, as in Go.** Each traversal
  (`!T`, `key(T)`, `!S`) starts at a random entry and goes round in
  insertion order, as Go's map iteration starts at a random point. Icon
  leaves the order unspecified (iconx's is the order of its hash table),
  and randomising it keeps programs from coming to depend on one; a
  program that prints a table unsorted gives different output from run
  to run. The randomness is not `&random`'s, so `?x` sequences are not
  disturbed. A traversal works on a snapshot of the keys: it produces
  each key present throughout once, skips keys deleted meanwhile, and
  may not produce keys added meanwhile. Set operations and `sort` use
  insertion order, so their results are deterministic. No reference
  test depends on iteration order; the suite passes with randomisation
  on. (The alternatives were insertion order, and reproducing iconx's
  hash order exactly.) `sort`
  follows iconx's `anycmp` order (null, integer, real, string, cset,
  file, co-expression, procedure, list, set, table, record; structures
  by serial number, records by type name then serial) and `sortf` its
  `nthcmp`. gcon sorts stably where iconx uses `qsort`, so elements that
  compare equal (such as table entries with equal values in `sort(t, 2)`)
  may come out in a different order.
- Output: iconx flushes the file at the end of every `write` call, so
  output from an interrupted call appears after the error message. gcon
  keeps standard output buffered but assembles each call's output, keeps
  an interrupted call's output for after the error message, and flushes
  standard output whenever something else could observe it: writing to
  `&errout` or a pipe, closing a pipe, reading `&input`, `system`, exit.
  Files the program opens are flushed per call, as in iconx.
- `read` ends a line at `\n`, `\r\n` or a lone `\r`, except in an
  untranslated (`"u"`) file, where only `\n` does and `\r` is kept, as
  iconx's own line reading does. `open`'s mode letters map onto C's
  `fopen` modes exactly as in iconx (`"rw"` and `"b"` update a file
  without truncating it).
- Limitation `e \ n` evaluates n first; when e's results run out, n is
  resumed and e starts again.
- Records: a declaration's global holds a `RecordType`, whose call makes a
  `Record`. Field references are looked up by name when they run, so
  `x.f` works on any record type with a field `f`; `r[i]` and `r["f"]`
  subscript by position or field name.
- gcon's extra built-ins (`len`, `runes`, `bytes`, `uchar`, `utf8valid`) and
  the keyword `&uset` are in the catalogue marked `Gcon`. Like any
  built-in, they are hidden by a declaration of the same name but not by
  an undeclared use, which would assign to the global. None of these
  names is used undeclared in the reference tree or `icn/` (`runes`,
  `uchar` and `utf8valid` do not occur at all). `text`, an earlier
  choice, was rejected: it is a natural variable name, used undeclared
  in `tests/general/diffwrds.icn`.
- Error conversion (`internal/vm/errors.go`) follows iconx's `err_msg`
  (`errmsg.r`). A run-time error is a Go panic carrying a
  `RuntimeError`; the main loop runs under one deferred `recover`, so
  the normal path pays nothing per instruction. When `&error` is
  nonzero, it is decremented, `&errornumber`, `&errortext` and
  `&errorvalue` record the error, and the instruction that raised it
  takes its failure label: the frame that was running (`vm.cur`) is
  still at that instruction, because the pc moves only after an
  instruction completes. The instructions that can raise errors all
  have failure labels (`OpOper`, `OpCall`, `OpResume`, `OpAssign`,
  `OpActivate`, and `OpScanEnter`, which was given one, resuming the
  subject expression). An error at any other instruction is reported
  as usual. `&errornumber` and `&errortext` fail when there is no error
  number; `&errorvalue` fails when the error had no offending value, as
  with `runerr(i)` called with only one argument (an explicit `&null`
  counts as a value). As in iconx, an interrupted `write` keeps the
  output it had produced. `errorclear()` and `runerr()` are as in
  `fmisc.r`. Checked against iconx by `testdata/errconv.icn`.
- String invocation (`internal/vm/invoke.go`) follows iconx's `invoke`
  and `strprc`: a value that is not a procedure is first converted to an
  integer, which selects an argument (so `"2"(a, b)` is `b`); otherwise
  it is converted to a string naming a global procedure, function or
  record constructor, or else (when it does not start with a letter) an
  operator, chosen by spelling and the number of arguments (`"-"(x)`
  negates, `"-"(x, y)` subtracts). Anything else is error 106. As icont's
  linker discards the globals that main does not reach (`lglob.c`,
  `scanrefs`), the compiler records which globals each procedure refers
  to and `Program.Invocable` marks those reachable from main or named
  in an `invocable` declaration; `invocable all` keeps them all. As in
  icont, the only identifier `invocable` accepts is `all`. The
  assignment operators invoked by name (`":="(x, y)`, `"<-"`, `":=:"`,
  `"<->"`) receive their arguments as variables, as iconx's operators
  with undereferenced arguments do (`Builtin.NoDeref`).
- `&line` is the line of the keyword; `&file` is the source path as
  given to gcon, as iconx's is the path given to icont. iconx looks the
  line up from its instruction pointer after the keyword instruction,
  so when icont's line table starts a new entry right there (it emits
  one for nearly every node whose line or column differs), `&line`
  reports the next entry's line: in `f(&line,` / `&line)` spread over
  lines 8 and 9, iconx gives 9 and 8. That depends on icont's code order
  and is not reproduced; on one line the two agree.
- **Large integers** (`internal/vm/bigint.go`), as iconx's `rlrgint.r`.
  An integer is an `Int` (int64) when it fits and a `*Big` (wrapping
  `math/big.Int`) otherwise. Results are always normalised, so a `Big`
  never holds a value that fits an `Int`: `===`, table keys and set
  membership compare `Big`s by value and never confuse the two types.
  Arithmetic on `Int`s keeps its fast path and redoes an operation in
  `math/big` only when it overflows (no measurable cost on an integer
  loop). Literals, numeric strings (decimal and radix) and
  `integer(r)` of a real beyond int64 make large integers; contexts that
  need a word-sized integer (positions, `to`, `seq`, list subscripts,
  `repl` counts: iconx's `cnv:C_integer`) give error 101, as iconx does.
  `seq` and `s[i+:j]` still raise 203 on overflow, as in iconx.
  Matching iconx in detail:
  - `/` truncates and `%` takes the sign of the dividend (C's rules);
  - conversion to real follows `bigtoreal`, summing base 2^32 digits from
    the top so that rounding agrees; a value too large for a real makes
    `real()` fail and is error 102 in mixed arithmetic;
  - `image()` shows `integer(~10^n)` when iconx's estimate of the number
    of digits (from its base 2^32 digits) is 30 or more;
  - `x ^ y` with y <= 0: `0 ^ y` is error 204 (including `0 ^ 0`), `1`
    and `-1` give 1 or -1, anything else 0; a large exponent with any
    other base is error 307, where iconx runs out of memory; a real to a
    large power is computed as `bigpowri` does, without an overflow
    check, so `2.0 ^ (2 ^ 100)` is `inf.0`;
  - `iand`, `ior`, `ixor`, `icom` and `ishift` act on two's complement
    values of unbounded width (`ishift(i, j)` is `i * 2^j` rounded down),
    which is what iconx's results are for large and small values alike.
  `&features` lists the features gcon has, with iconx's names. `lgint`, `overflow`, `arith`,
  `radix` and `over` pass; `large` matches except for its `&trace`
  output. `testdata/bigint.icn` checks the edge cases against iconx.
- `?x` and `&random` (`internal/vm/random.go`) reproduce iconx's
  generator (`oref.r`, `RandVal`): `&random` starts at 0 and steps as
  `(1103515245 * &random + 453816694) mod 2^31`, scaled by iconx's
  `RanScale`, so a program that seeds `&random` gets iconx's results for
  integers (large ones by `bigrand`'s digit-by-digit method), strings,
  csets, lists and records. Elements of lists, records and tables are
  produced as variables (`?L := x`). Tables and sets choose by position
  in insertion order where iconx uses its hash order, so `?T` and `?S`
  differ from iconx even with the same seed. A random character of a
  string held in a variable is a substring variable, as `s[i]` is.
- Reversible assignment: `x <- y` and `x <-> y` save the old values
  before assigning; resumed, they restore them and fail (iconx's `rasgn`
  and `rswap`). The compiler emits them inline, as it does `:=:`. As in
  iconx, each assignment that fails makes the operator fail at once, so
  `x :=: &pos` with an invalid x leaves x assigned and &pos not, and a
  resumed `&pos <-> x` whose `&pos := old` fails (the subject having
  been shortened meanwhile) leaves x unrestored (`subjpos`).
- Procedures return and suspend variables, as iconx's `retderef`
  decides: a variable in the procedure's own frame (a local or
  parameter) is dereferenced, since the frame is going away; globals,
  statics, structure elements and keywords are produced as variables,
  so `f(x) := y` assigns through what `f` returned.
- **Tracing** (`internal/vm/trace.go`), as iconx's `rdebug.r`. While
  `&trace` is nonzero, each procedure call, return, suspension, failure
  and resumption, and each co-expression activation, return and failure,
  writes a line to `&errout` and decrements `&trace` (negative traces
  indefinitely); `&trace` starts at the environment variable `TRACE`, so
  main's call can be traced. Built-in functions are not traced. Lines
  are `file: line  ` (the file's last 13 characters), then `| ` for each
  `&level`; calls show the arguments as the procedure receives them
  (padded, or gathered into a variadic list). `&level` counts procedure
  calls: up on a call or resumption, down on a return, suspension or
  failure, unchanged by co-expression switches. The lines match iconx's
  because each leaving instruction carries the line of its keyword
  (`return`, `suspend`, `fail`, or `end` for falling off the end) and a
  call carries the line of its `(`. Values are shown by `OutImage`,
  iconx's `outimage`, which run-time error messages use as well: strings
  past 16 characters and csets past 16 members end in `...`, lists show
  their elements (`list_1 = [1,2,3]`, at most 3 at each end), records
  their fields, and inside them lists and records are shown by size;
  variables appear as `(variable = ...)` or `&pos = ...`.
- **Run-time error messages** are iconx's (`errmsg.r`, and `tracebk`
  and `ttrace` in `rdebug.r`): the header, the offending value by
  `OutImage`, then a traceback of the active calls from the outermost,
  each with its parameters' current values and " from line n in file"
  (main has none; the base of a co-expression shows where its creating
  procedure was called from), and last the failing operation: `{x + y}`,
  `{x[i]}`, `{x[i:j]}`, `{x to y by z}`, `{x ? ..}`, `{v @ c}`,
  `{x . field}` (iconx prints the word "field"), `limit counter: n`,
  `f(args)` for a call (by the name a string resolved to, if it did), and
  so on. Details reproduced: operands that iconx's operators take as
  variables (the subscripted value, the targets of assignments, `!x`,
  `?x`) show as `(variable = ...)`; the left operand of a binary
  operator shows as already converted (`{"5" || list_1 = []}` for `5 || []`), as
  iconx converts in place; a built-in generator failing on resumption
  shows its call. To match, `x[i+:j]` compiles to `i + j` then a section,
  as icont does (so `s[1+:"z"]` is error 102), and `:=:` and `<->` check
  that both operands are variables before assigning either. Checked by
  the programs in `testdata/traceback`, each one error.
- **Substring variables** (`internal/vm/substr.go`), iconx's substring
  trapped variables: `s[i]`, `s[i:j]`, `s[i+:n]`, `?s` and `!s` produce
  a `SubstrVar` (the variable, a byte position and a length) when `s` is
  a variable holding a string, not merely one convertible to a string
  (`s := 12345; s[2] := "x"` is error 111, as in iconx). Assigning to it
  converts the value to a string, replaces that part of the string,
  assigns the whole back to the variable (assigning to `&subject` this
  way resets `&pos`) and takes the new length; dereferencing checks the
  string is still a string and long enough (errors 103, 205). A
  substring of a substring refers to the original variable. The
  operators that make them (`Builtin.RefFirst`) receive their first
  operand as a variable. When `:=:` or `<->` exchanges two substrings of
  one string, each assignment shifts the other substring, if it is to
  the right, by the change in length (the compiler records the partner
  operand on each assignment), which reproduces iconx's `adj1`/`adj2`
  in `swap` and `rswap`, forward and when `<->` is resumed.
- `p ! L` calls `p` with the elements of list or record `L` (error 126
  otherwise), as iconx's `Op_Apply`: an `OpCall` marked `ApplyOp`.
- Repeated alternation `|e` (also written `||e`, `|||e`, as icont reads
  them) generates e's results over and over until a round produces none.
- Prefix tokens that are sequences of unary operators apply right to
  left, as icont's `unopb` emits them: `++x` is `+(+x)`, `~==x` is
  `~(=(=x))`.
- `x @:= c` is `x := x @ c`, and `x &:= y` is `x := y` with x evaluated
  first; the other augmented assignments are `x := x op y` as before.
- **Procedures as values** (`internal/vm/names.go`): `proc(s, i)` finds
  what string invocation would (`i` operands, default 1; `i = 0` only
  built-in functions); `args(p)` is iconx's `nparam`, taken for built-in
  functions from iconx's own reports (a variadic function with n fixed
  parameters gives -(n+1)); `function()` generates gcon's built-in
  function names. A call of a built-in written in the program has its
  arguments fitted to the parameters as iconx's `invoke` does (padded
  with `&null`, extras dropped), so `"~==="()` or `image()` work.
- **Variables by name**: `name(v)` as iconx's `get_name` (identifiers,
  keywords, `L[i]` with a literal "L", `rec.field`, `T[key]`,
  `x[i:j]`); `variable(s)` as `getvar` (keywords, then the running
  procedure's parameters, locals and statics, then globals the linker
  kept). Procedures record their local names (`Proc.LocalNames`), in
  iconx's order: parameters, locals as declared, implicit locals by
  first use, then statics (`Proc.Statics`). `display(i, f)` lists the
  current co-expression, the identifiers of the `i` innermost
  activations, and the kept globals, sorted, as `xdisp`.
- `open` of a directory for reading reads its entries (with "." and
  "..", sorted): `read` and `!f` an entry, `reads(f, n)` an entry cut to
  n bytes; `seek` and `where` fail, as iconx's `ReadDirectory`.
  `x[]` is `x[&null]`, as icont compiles an empty subscript list.
- The keyboard functions `getch`, `getche` and `kbhit` (`kbd*.go`) put a
  terminal on standard input into non-canonical mode while they run, as
  iconx's `rsys.r` does, using the standard library's `ioctl` on macOS
  and Linux; they read through `&input`'s buffer, so `kbhit` leaves what
  it sees for the next read. Off a terminal they read standard input
  as it comes (`kbhit` succeeds there at end of file, as `select` does).
  `collect` checks its arguments and does nothing (memory is Go's);
  `chdir` and `delay` are as iconx's.
- Not provided: `loadfunc` (dynamic loading of C functions; error 121),
  and `fattrib`, which iconx's Unix builds leave out.
- Programmer-defined control operations: `p{e1, ..., en}` is
  `p([create e1, ..., create en])`, as icont's `Pdco` rules build it
  (an empty `ei` is `create &null`; `p{}` passes an empty list).
- **Keywords** (`internal/vm/keywords.go`), as iconx's `keyword.r`:
  `&clock`, `&date` and `&dateline` in iconx's formats; `&time` is
  processor time in milliseconds (`getrusage`, as iconx's `millisec`);
  `&host`; `&version` is "Icon Version 9.5 (gcon)"; `&progname` is the
  name the program would have if built by icont and run from its
  directory (`./kwds` for `kwds.icn`, as iconx's is its `argv[0]`), and
  assignable; `&dump`, when nonzero at the end (normal, `exit`, `stop`
  or a run-time error), writes iconx's termination dump; `&eventcode`,
  `&eventsource` and `&eventvalue` are plain variables; the mouse-event
  codes (`&lpress` .. `&resize`) are iconx's constants; `&x`, `&y`,
  `&row`, `&col`, `&interval`, `&control`, `&shift`, `&meta`,
  `&window` and `&column` fail, as in an Icon without graphics.
  The storage keywords describe iconx's regions, which gcon does not
  have: `&collections` counts `collect()` calls as iconx counts
  collections (total, and by region), which are the only collections
  in a program that does not fill iconx's regions; `&regions` gives
  iconx's initial sizes (0, 500000, 500000); `&storage` and
  `&allocated` give zeros, where iconx reports bytes used.
- Operators that produce at most one result are `OpOper` instructions
  calling a Go function; generators (`to`, `!`, built-in generators)
  and procedure calls use `OpCall` and `OpResume` with a handle in a
  temporary, which holds a suspended frame or a Go `Generator`.

### Deliberate simplifications (to be removed)

- `&allocated`, `&storage` are zeros (see the keywords above), so the
  IPL's `large()`, which detects large integers by the allocation they
  cause, never succeeds (the `ilib` test).

## Verification

- Oracle: `icon/tests/general` has 99 `.icn`/`.std` pairs. gcon's
  conformance test runs each program and compares its output with the
  `.std` file. Programs that do not yet pass are listed explicitly, so
  progress shows up in the diff.
- Program tests (`internal/run/testdata`): each `FILE.icn` has its
  expected standard output and exit status recorded from iconx by
  `tools/iconx/expect.sh`; a program with a line `#STDERR` also has its
  standard error recorded (`FILE.err`) and compared, which is how trace
  output is checked.
- New syntax and semantics facts go here, with the C source or test that
  shows them.
- Differential testing (`tools/difftest`): for programs with no
  recorded output, such as the IPL's, each program is built by icont
  and by gcon and run under both, as `./NAME` in its own directory, with
  its own source as standard input; output, standard error and exit
  status are compared, and a program whose iconx output differs between
  two runs is set aside as nondeterministic.

  ```
  go run ./tools/difftest -iconx $(tools/iconx/build.sh) -gcon bin/gcon ../icon/ipl/progs/*.icn
  ```

  On `ipl/progs` (2026-11-25), 247 of 275 programs behave identically.
  The first run found five bugs, now fixed and covered by program tests:
  a pipe's `close` returned the wait status, not the exit code; a
  command read through a pipe got no standard input; a pipe left open
  at exit lost its command's output (gcon relayed the command's output
  and died first); commands ran as "/bin/sh", not "sh"; and runaway
  recursion had no limit (error 301). Tracebacks also now show a
  built-in function's arguments adjusted to its arity, and the
  arguments of a call of a non-procedure as variables, as iconx does.
  The remaining differences are understood:

  | Program | Difference |
  |---|---|
  | `gftrace` | prints `&version` |
  | `ifncsgen` | `function()` includes gcon's Unicode functions and not `loadfunc` |
  | `lisp` | overflows the stack at another depth (see "The evaluation stack") |
  | `normalize` | iconx gives `p ! L` the previous location: icont's `N_Apply` emits none (tcode.c); not copied |
  | `when` | parses `ls -al` columns of its own directory, whose built files differ in size |
  | `htget`, `weblinks` | link `cfunc`, C functions loaded by `loadfunc` |

  Eight programs are nondeterministic under iconx (clocks, dates,
  orphaned processes) and 13 do not finish with their source as input.

- Performance (`icon/tests/bench`, 2026-11-25, Apple M-series, both
  programs built). The five benchmarks give identical output; whole
  programs run from twice as fast to 2.5 times slower than iconx, and the
  IPL programs above at a median 1.07 times iconx's time, being
  dominated by start-up and I/O. `micro.icn` (from Jcon), which times
  each of 108 operations in a loop, shows where the time goes:
  - gcon wins co-expression activation by two orders of magnitude (21
    ns to 2611: iconx on macOS switches co-expressions through
    threads), and conjunction (`1&2&3&4&5`, compiled to jumps);
  - I/O, global access and real conversion are level;
  - most operations take three to five times as long (geometric mean
    3.5): the cost of each instruction, in which boxing integers into
    `Value` (`any`) is a likely part;
  - some string functions took ten to seventy times as long (`map`,
    `reverse`, `detab`, `entab`, `left`, `right`, `center`, `trim`).
    The cause was not UTF-8 but how they handled characters: each
    split its strings into a `[]string` of one-character strings and
    joined the result, and `map` built a Go map from its arguments on
    every call. They now work on bytes when every character is one
    (bytes strings, and ASCII text, which a string records), use byte
    offsets otherwise, build results directly, and `map` keeps its last
    table as iconx does (a VM's `mapTab`); a cset caches its string
    form, which `map(s, &lcase, &ucase)` needs on every call. The
    functions alone are two to 27 times faster
    (`internal/vm/strfuncs_test.go`), and in `micro.icn` they are now
    2.4 to 7 times iconx's time, the same band as other operations; the
    geometric mean over all operations went from 3.51 to 2.88.
  - The cost of each instruction (`go test ./internal/run -bench VM`
    times single operations through the VM). First round:
    - Arguments of calls that do not keep them (functions other than
      generators, procedures, record constructors) go in a scratch
      buffer the VM reuses, not a new slice per call (`scratchArgs`).
      `GCONDEBUG=poisonargs` overwrites the buffer after every call, so
      a function that kept it fails visibly; the whole test suite
      passes so, and a deliberately broken list literal is caught.
    - Integer arithmetic and comparison are computed inline when both
      operands are Ints (`fastInt`, via the operator's `intOp`),
      falling back to the operator's function for anything else.
    - Boxing an Int into a Value allocates beyond 255; `mkInt` hands
      out boxes for -256 to 4095 made together at start-up. (Boxes made
      lazily over a wider range measured slower: scattered over the
      heap, they missed the cache when a counter ran through them.)
    - Serial numbers are plain counters, not a map lookup by type name.
    - Tuning the garbage collector (GOGC) changed nothing measurable.

    `x + 407` went from 30.5 to 13.3 ns with no allocation, and in
    `micro.icn` integer arithmetic and comparison are now level with
    iconx; the geometric mean over its operations went from 2.88 to
    2.36 times iconx's time. What remains is structural: Ints beyond
    the boxed range and every real still allocate when stored in a
    Value; operators and functions go through a general call that
    converts their operands; a subscript or field always makes a
    variable, even where only its value is used; and `to` and other
    generators go through an interface call per result.

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
5. `gcon build` producing an icont-style script. Done (see "The built
   program's format"), with `gcon dis`.
6. The IPL in a gcon root, found as Go finds GOROOT. Done (see
   "Decision: a gcon root, as GOROOT"), with `gcon env`.
7. A differential test of IPL programs against iconx.
8. `&allocated`, `&storage`, `&regions` and `&collections` from Go's
   memory statistics; record that `loadfunc` is not supported.
9. A distribution section, with packaging when graphics arrives.
10. gcon-vm, the VM in C (see "Decision: the VM in C"), reading the
    built-program format; the Go VM stays as the reference meanwhile.
