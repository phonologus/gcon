# gcon-vm design

gcon-vm runs gcon's bytecode. gcon (Go) is the toolchain: it translates
Icon to bytecode, writes built programs, formats, serves the language
server; gcon-vm (C) executes. gcon-graphics (C) is the display server.
C builds stay out of the Go repository.

## Decision (2026-11-25): the VM in C

gcon's Go VM reached 1.6 times iconx's time over `micro.icn` (from 3.5),
beating iconx on loops, integer arithmetic, comparison and co-expression
switching, but the rest of the gap was structural, and closing it meant
working against Go's runtime: values as tagged structs needed `unsafe`;
every store of a value (which holds a pointer) into a frame, list or
table pays a write barrier while the collector marks; frames that may
outlive a call must be on the heap; dispatch is a `switch`, with bounds
checks; allocation goes through the collector rather than a bump
pointer. See gcon's `docs/design.md` ("Decision: the VM in C") for the
stock-take.

Extending iconx instead was considered and set aside. iconx switches
co-expressions through POSIX threads and semaphores (`rswitch.c`), and
its interpreter recurses on the C stack, so fast co-expressions would
need per-architecture stack-switching assembly; and its strings are byte
runs and its csets 256-bit maps throughout a 26,000-line runtime, so
gcon's Unicode semantics would mean surgery across most of it. gcon's
VM design (Proebsting's translation, a non-recursive interpreter) gives
cheap co-expressions by construction, and its semantics are done and
tested (84 of 89 reference tests, 247 of 275 IPL programs identical to
iconx), so gcon-vm ports that design.

**Evidence** (`experiments/spike`, Apple M-series, `cc -O2`): the loops
of gcon's `experiments/valuerep`, with the same instructions and frame
layout, and a procedure call with frames on a stack:

| loop | C, computed goto | C, switch | Go, same design |
|---|---|---|---|
| `x := i + 407` | 2.1 ns | 2.3 | 3.7 |
| `v := L[3]` | 2.7 | 3.1 | 5.2 |
| `r := r + 1.5` | 3.8 | 3.8 | 4.0 |
| call `q(i, n)` | 3.5 | 3.4 | 7.4 |

C is about twice as fast where dispatch and stores dominate (the real
loop is bound by the floating-point add's latency in both); computed
goto gains about 10% over a switch. The Go VM's own call costs 45 ns:
most of the difference from 7.4 is design (heap frames, generality),
which gcon-vm avoids as below.

## Plan

- **Input.** gcon's built-program format (`gcon/internal/vm/encode.go`,
  `docs/design.md` "The built program's format"): versioned, sectioned,
  checksummed, with builtins by name. gcon-vm's loader checks it as
  `vm.Decode` does. gcon passes a program to gcon-vm (`gcon run`
  translates then hands the bytecode over; a built program's header runs
  gcon-vm), found in `$GCONROOT/libexec`.
- **Values.** 16-byte tagged structs: a kind, flags and a 48-bit field
  (a string's length, an index), and a word (an integer, a real, a
  pointer). Kinds as gcon's `valuecore.go`, variables included, so a
  number, a substring or a variable allocates nothing.
- **Frames and co-expressions.** Icon's generators are resumed in last-in
  first-out order, so a suspended procedure's frame stays on the stack
  below its caller's later frames, as in iconx; each co-expression has
  its own stack, and since the interpreter never recurses in C,
  switching co-expressions is switching the current stack.
- **Memory.** A precise collector: the roots are the co-expression
  stacks, globals, statics and constants, all known. Start with mark and
  sweep over size-classed blocks and a bump-allocated string region;
  measure before refining.
- **Dispatch.** Computed goto, with a switch for compilers without it.
- **Built-in functions.** Ported from gcon's Go VM, keeping its Unicode
  semantics (units, csets with code-point ranges); large integers and
  real formatting from iconx's public-domain sources where they fit.
- **Graphics.** A client of gcon-graphics' protocol.
- **Testing.** gcon's program tests, the reference conformance tests and
  the differential test against iconx, and the Go VM as a second oracle:
  both VMs run the same built file. The Go VM remains the reference
  until gcon-vm passes everything.

## The loader (2026-11-25)

`src/load.c` reads gcon's built-program format, format version 1, as
gcon's `vm.Decode` and `vm.Validate` do: the same reads in the same
order, the same checks, and the same messages, so gcon-vm and gcon
report a damaged file identically. It is a transliteration, by design:
as in gcon, a decoder records its first error and every later read
returns zero, so the code reads straight through and checks once.

- **Where the program is.** A built program (a `#!` file in which a
  line holding `[gcon program follows]` is followed by the magic), or a
  file that is just the encoded program (starting with the magic), the
  form in which `gcon run` will hand over bytecode it has translated.
- **What it makes.** A `Program` close to gcon's: procedures with their
  instructions (one struct per instruction, fields as gcon's `Instr`),
  record types, constants, globals with their names and initial values,
  main, the invocable set and the build information. Constants are
  values in the 16-byte layout planned above (a header holding kind,
  flags and a 48-bit field; a word); strings point into the file's
  bytes, which stay loaded. Translating instructions to the form the
  interpreter dispatches on is the interpreter's business, done when it
  exists.
- **Builtins** stay references (a kind and a name), checked against the
  names gcon knows, so an unknown one is refused at load as gcon refuses
  it. The names are generated from gcon's sources
  (`tools/gentables.sh`, through a `go run -overlay` that adds a command
  to gcon's module without changing gcon). When gcon-vm has its own
  builtins, the check becomes "names gcon-vm implements".
- **Messages** are buffers with a length, not C strings: they quote
  names from the file, which can hold any bytes, NUL included. (The
  damage test found this: a builtin name with a NUL in it.)

**Listing.** `gcon-vm -dis` writes exactly what `gcon dis` writes, so the
two can be compared byte for byte. That needs gcon's images of
constants, which the VM needs anyway for `image()`, `string()` and
`write()`: `src/image.c` ports gcon's `quote` (Icon's escapes for ASCII;
in text, Go's rule: printable characters as themselves, others as `\u` or
`\U`), cset images (named csets, members, or ranges past 256 members),
`rtos` (C's `%.10g`, which Go's `'g'` with precision 10 matches), and
large integers (digits, or `integer(~10^n)` from iconx's estimate).
Printability is Go's `strconv.IsPrint`, as a generated table of the
non-printable ranges (741 ranges, Unicode 17.0.0): gcon's images follow
the Unicode tables of the Go that built it, so the table is regenerated
with the builtin names.

**Tests.** `tests/dis.sh` builds programs with gcon and compares
listings: 483 programs (the reference tests, the IPL programs, gcon's
program tests, the samples in `icn/`, and `tests/values.icn`, whose
constants exercise every image case) are identical. `tests/damage.sh`
compares every truncation of a built program and every byte changed
two ways; after a change it makes the checksum right again, so the
change reaches the decoder's other checks and the validator instead of
stopping at the checksum, and some variants are valid programs whose
listings are compared. Over `tests/values.icn` and `icn/bbox.icn`,
21,597 variants give identical output, messages and exit status, with
gcon-vm built with the address and undefined-behaviour sanitizers; the
messages cover every check in the decoder and most in the validator.

## Planned (2026-11-25): `loadfunc`, revisit when the VM runs

gcon does not provide `loadfunc` (error 121) because gcon is pure Go,
without cgo. gcon-vm is C and can `dlopen`, so it can bring `loadfunc`
back. Take this up once the interpreter runs and the builtins are in.

- **Interface: source compatibility with 9.5's `icall.h`**
  (`icon/ipl/cfuncs`), not binary compatibility. A C function is
  `int f(int argc, descriptor *argv)`, returning 0 (success, result in
  `argv[0]`), -1 (failure) or an error number. gcon-vm ships its own
  `icall.h` and provides the callbacks its macros use (`cnv_int`,
  `cnv_real`, `cnv_str`, `cnv_c_str`, `getdbl`, `alcstr`, `alcreal`,
  `alcfile`). A shim converts arguments to iconx-style descriptors and
  converts the result back, so existing `cfuncs` code recompiles
  unchanged. Libraries built for iconx are not loaded: they depend on
  iconx's block layouts, and matching those would tie gcon-vm's value
  representation to iconx's.
- **To decide then:** the unit of a string a C function returns (bytes,
  or by content as a literal's); whether the shim copies strings (so C
  code holding pointers cannot be hurt by the collector, a hazard iconx
  ignores); external values (`T_External`, with their function lists)
  as a new kind.
- **The two VMs differ** until gcon-vm replaces the Go VM: under the Go
  VM `loadfunc` stays error 121. No format change is needed: `loadfunc`
  is already one of gcon's function names.
- **Gains:** the IPL programs that need C functions (`htget`,
  `weblinks`) can run, and native bindings can be added without growing
  the VM.

## The interpreter (2026-11-25)

gcon-vm runs programs: `gcon-vm prog args...` loads a built program,
links it and runs it, with `&progname` the name it was given, as
iconx's is the icode file's. The run-time system is a port of gcon's Go
VM (`internal/vm`), file for file, keeping its semantics exactly: the
Go VM is the reference, and the tests compare the two.

| gcon (Go)                         | gcon-vm (C)        |
|-----------------------------------|--------------------|
| vm.go, coexpr.go, errors.go       | `src/interp.c`     |
| value.go, valuecore.go, substr.go | `src/conv.c`       |
| ops.go, invoke.go, random.go      | `src/ops.c`, `src/num.c` |
| bigint.go (math/big)              | `src/big.c`        |
| cset.go                           | `src/cset.c`       |
| structs.go, record.go             | `src/struct.c`     |
| scan.go                           | `src/scan.c`       |
| strfuncs.go                       | `src/strfn.c`      |
| file.go, kbd.go                   | `src/file.c`       |
| trace.go, names.go, keywords.go   | `src/trace.c`, `src/builtins.c` |

- **Values** are the 16-byte layout above. Strings keep their unit and
  ASCII flag in the header's flags, their length in its 48-bit field;
  list elements and record fields as variables are the structure and
  an index, so subscripting allocates nothing.
- **Frames and control** are as in the Go VM: frames chained by caller;
  a suspended procedure is a frame held in a temporary of its caller; a
  co-expression is a chain of frames, and switching co-expressions
  switches the running chain. Frames are recycled when they return
  (below, "Tuning").
- **Run-time errors** unwind by `longjmp` to the loop's caller, which
  converts the error to failure at the failing instruction if `&error`
  allows (as the Go VM's `recover` does), or reports it with iconx's
  message and traceback.
- **Builtins** are resolved by name at link time, from tables each
  source file registers; generators are structs with a `next` function,
  and builtins that restore `&pos` when resumed (`tab`, `move`, `=s`)
  keep the old position in the handle, allocating nothing.
- **Large integers** (`src/big.c`): sign and magnitude in base 2^32
  digits, as iconx's, with Knuth's division; every result normalised,
  so a large integer is never equal to a small one. Conversions to and
  from reals follow iconx digit by digit, as gcon's do.
- **Files** are buffered by gcon-vm itself on file descriptors (so a
  read from a terminal returns what is there). One difference from
  gcon: at exit, gcon-vm waits for the commands of pipes still open for
  writing, so their output is complete when it ends; gcon waits only
  when it relays their output, and iconx never waits.
- **Memory** is managed by the collector (below, "The collector").

**Tests.** `tests/programs.sh` runs gcon's program tests (expected
output, exit status and standard error recorded from iconx) built, as
gcon's own tests run them: all 93 pass. `tests/vs-go.sh` runs programs
on both VMs from the same built file and compares output, messages and
exit status: the 89 reference tests gcon builds are identical.
`tests/bench.sh` times gcon's VM micro-benchmarks on both.

**First measurements** (Apple M-series, `cc -O2`, no tuning, ns per
iteration, Go VM then gcon-vm): empty loop 6.6 / 4.8, `x + 407` 11.3 /
7.6, `p()` 26.0 / 17.9, `q(1, 2)` 50.8 / 27.5, `L[3]` 19.7 / 11.8,
`insert(T, 5) & delete(T, 5)` 106.5 / 29.4, `left("a", 10)` 66.5 / 49.1,
`map(...)` 48.5 / 62.0. Faster on all but `map` before any tuning, and
without a collector's costs, which will change these.

**The IPL** (`tests/vs-go.sh ../icon/ipl/progs`, each run limited to
ten seconds): 260 programs identical on both VMs; 3 nondeterministic
(their two Go runs differ); 9 that run past the limit on the Go VM
(most wait on input that never ends a loop); 2 gcon does not build.
One, `envelope`, differs only in timing: it writes to a pipe into
`lpr`, whose message gcon-vm waits for before exiting and gcon does
not. A program that seeds `&random` from the clock (`ilib`, through
the IPL's `randomize`) is caught as nondeterministic only when its two
Go runs fall in different seconds.

**Co-expressions** (`tests/bench.sh`, ns per operation, iconx / Go VM /
gcon-vm): `@C` round trip 2,659 / 42.3 / 24.5; `@(create 1)` 17,488 /
150.7 / 77.9. Over the other fourteen micro-benchmarks gcon-vm's
geometric mean is about 0.82 of iconx's time and 0.65 of the Go VM's,
before tuning and without a collector; `left` (1.7 times iconx) and
`map` (2.1) are the outliers.

## The collector (2026-11-25)

`src/gc.c`: precise mark and sweep, as planned above ("Memory").

- **Safe points.** A collection runs only at the top of the
  interpreter's loop, between instructions. There no C code holds a
  heap pointer the roots do not, so builtins allocate freely (their
  temporaries are simply garbage afterwards) and nothing on the C
  stack need be found. Allocation sets a flag when the threshold is
  passed; the loop tests it.
- **Roots** are all known: the globals, the constants, the running
  frame and co-expression and `&main` (frames reach their callers;
  suspended frames, co-expressions and generators are reached through
  the values that hold them), a few fields of the VM (`&subject`, the
  scratch arguments, the error values, the files, `map`'s table), and
  statics registered with `gcroot` (the predefined csets).
- **The heap** is 1 MB chunks, aligned, each holding blocks of one size
  class (16 bytes to 32 KB), and larger objects in chunks of their own.
  A hash of megabyte addresses finds the chunk of any pointer, so a
  pointer into the middle of an object (a substring's bytes, a
  variable that is a local of a frame, a generator over a table)
  finds and keeps the whole object; a pointer outside the heap (a
  constant's bytes in the loaded file, a C string) is ignored. Every
  block has a type, from which the collector knows how to trace it;
  generators (and `map`'s table) carry their own mark function. Marking
  uses an explicit stack, so deep structures do not recurse in C.
- **Sweeping** puts dead blocks on per-class free lists and frees dead
  large objects. The next collection comes when as much has been
  allocated as was live after this one, and at least 16 MB.
- **Constants** stay outside the heap, but a cset constant's cached
  string is in it, so marking a constant cset traces its fields.
- **Testing.** `GCONVMSTRESS=n` collects at the first safe point after
  every n allocations and poisons freed blocks, so that a missing root
  shows up as wrong output or a crash: the program and reference tests
  pass with `n` = 1, also with ASan and UBSan (which see each chunk as
  one allocation, so the poisoning stands in for them inside the
  heap). `GCONVMDEBUG=gc` reports each collection.

**Measured.** A loop making two million strings, lists and table
entries runs in 20 MB (291 KB live at each collection). With a
co-expression made and activated in each iteration as well, everything
stays live: `create` copies the procedure's locals, including the
variable holding the previous co-expression, so each keeps the one
before reachable. The Go VM keeps the same chain (2.8 GB); gcon-vm
finishes in 0.9 s and 2.2 GB; iconx fails at once ("cannot create
thread"). Copying only the locals a co-expression's body uses would
break such chains; that is the compiler's to decide (gcon), and is
observable only through `variable` and `display` in the body, which
would then not find the locals left out.

The micro-benchmarks barely move: `@(create 1)` goes from 77.9 to
47.8 ns (memory is reused rather than touched fresh), the rest within a
few per cent.

## Tuning (2026-11-26)

Four changes, measured one at a time with `tests/bench.sh`:

- **Strings are built in the heap** (`SB` in `src/vm.h`): the buffer
  becomes the string, uncopied, which is safe because the collector
  runs only between instructions. String bytes are allocated unzeroed
  (`gcraw`), and a result's ASCII flag is passed on when already known
  (`mkstrf`) rather than found by scanning. `left`, `right` and
  `center` write one-byte characters straight into a block of the
  result's size; `map` with a byte table likewise.
- **Link-time optimisation** (`-flto`): the small functions that every
  operation calls across files (`deref`, `cvstr`, `strval`, the ASCII
  test) are inlined. `left` went from 1.7 times iconx's time to parity
  with these first two.
- **Frames are recycled, not stacked.** A frame per co-expression stack
  in the strict sense does not fit gcon's code: a suspended generator's
  frame must stay while its caller goes on calling, and iconx reclaims
  the stack above abandoned generators when a bounded expression ends
  (its mark and unmark), which gcon's instructions do not record (an
  abandoned generator's handle is simply overwritten). So `while x :=
  gen()` would leave a dead frame on the stack per iteration. What a
  stack gives, cheap last-in first-out allocation of memory still in
  the cache, comes instead from recycling: a frame that returns or
  fails without ever having suspended goes on a free list by its number
  of slots, and the next call of that size reuses it at once. Nothing
  else can refer to it: arguments are dereferenced, `return`
  dereferences locals, co-expressions copy values. The one escape, a
  substring of a local returned (`return s[2]`), is detected and left
  to the collector, as are frames that have suspended. A collection
  forgets the free lists (the sweep frees those frames). Allocation
  finds a block's chunk by masking its address (each chunk begins with
  a pointer to its header), and frames are initialised directly, not
  zeroed first. Under `GCONVMSTRESS` a returned frame is poisoned
  instead of recycled, so that any later use shows: the tests pass so,
  under ASan and UBSan (the collector may still trace a stale
  reference to one, from the scratch arguments or an old temporary,
  and skips it; none is read).
- **Computed goto**: each handler dispatches the next instruction
  itself; a switch is used where the compiler lacks the extension
  (`-DGCONVM_SWITCH` forces it; the tests pass both ways).

| benchmark | iconx | gcon-vm before | after | after / iconx |
|---|---:|---:|---:|---:|
| empty loop | 7.4 | 3.3 | 2.8 | 0.38 |
| `*"abcde"` | 10.1 | 5.8 | 4.2 | 0.41 |
| `upto` | 40.6 | 43.6 | 39.0 | 0.96 |
| `find` | 39.7 | 42.0 | 37.6 | 0.95 |
| `x + 407` | 15.3 | 6.9 | 6.0 | 0.39 |
| `p()` | 15.8 | 17.4 | 9.6 | 0.61 |
| `q(1, 2)` | 31.1 | 22.0 | 15.8 | 0.51 |
| `L[3]` | 15.0 | 11.0 | 9.5 | 0.63 |
| `left("a", 10)` | 26.2 | 49.7 | 25.2 | 0.96 |
| `map(...)` | 27.7 | 62.5 | 25.5 | 0.92 |
| `move(1)` | 36.1 | 28.5 | 25.0 | 0.69 |
| table insert/delete | 37.3 | 25.8 | 24.1 | 0.65 |
| `pop(push(L, 0))` | 27.9 | 19.6 | 17.8 | 0.64 |
| same, empty list | 27.9 | 21.6 | 18.7 | 0.67 |
| `@C` | 2615 | 17.8 | 14.7 | 0.006 |
| `@(create 1)` | 17522 | 47.8 | 41.9 | 0.002 |

(ns per iteration; "before" is the collector's commit.) gcon-vm is now
faster than iconx on every one; the geometric mean over the first
fourteen is about 0.64 of iconx's time, and 0.51 of the Go VM's.
`upto` and `find` are nearest parity: the cost there is the call and
the generator, which a later pass can take on.

## Running from gcon (2026-11-26)

`gcon run` hands programs to gcon-vm (gcon's `docs/design.md`,
"Decision: gcon run uses gcon-vm"): a built program by `exec`ing
`gcon-vm program args...`; source by translating it, writing the
encoded program to a file deleted at once, and `exec`ing `gcon-vm
-progname name /dev/fd/N args...`, so that `&progname` is what the Go
VM gives the source. gcon finds gcon-vm through `$GCONVM`, or in its
root's `libexec`, where `tools/install.sh` puts it; with neither, or
`GCONVM=go`, it uses its Go VM. The loader accepts either a built file
(the shell header, the marker, the encoding) or the bare encoding.

gcon's `TestExecVM` runs all its program tests from source through the
hand-over, when `GCONVM` names a gcon-vm: all 94 pass. A built
program's header runs gcon-vm directly (the gcon-vm of the building
gcon's root, unless `GCONVM`, `GCON` or a gcon beside the program says
otherwise), without starting gcon: hello world starts in 6.6 ms, of
which gcon-vm is 4.2, against 11.2 for icont's output on iconx.

## Tuning on a real program: phwbi (2026-11-26)

`phwbi` (Sean's prism analyser, in `icn/`) on `test_big.wav` (732,410
samples) is the first whole-program benchmark: it times its
initialisation (reading the file into a list of records) and its main
pass (`prism()`), both before its window opens, so it runs headless.
Its inner code is records in a global list, almost every statement of
the form `mem[a].f := mem[b].g`. Profiled with macOS `sample` on a `-g
-fno-omit-frame-pointer` build; each change measured alone, A/B.

- **Field lookup remembered.** A field operator (one per field name,
  shared by every use of it) compared the name with each of the record
  type's field names, by `memcmp`, at every `r.f`: about a fifth of the
  time. It now keeps the field's index in each record type, by type
  number (record types are fixed at load), filled on first use
  (`fieldindex`). Main pass 2,400 to 1,890 ms.
- **`deref` inline.** It was a call for every operand, though most
  values are not variables; now `vm.h` tests for a variable inline and
  reads a `KRef` there, calling `derefvar` for the rest. 1,890 to
  1,710 ms, and initialisation 238 to 205 ms.
- **Fast paths for `L[i]` and `r.f`**, beside the integer ones in
  `operint`: a list with an integer subscript, and a record with the
  field, are done in the loop without the scratch buffer, the call
  through the builtin, or `mustint`; anything else falls back to
  `o_index` and `fieldfn`, so errors and the other cases are unchanged.
  1,710 to 1,270 ms.
- **`:=:` and `<->` by flag**: `OpAssign` compared the operator's name
  with two `strcmp`s on every assignment with an operator; `mkasgn` now
  sets `swap`. Within noise here, but free. (An inline `assignvar` fast
  path was tried too: no measurable gain, so not kept.)

| (ms) | iconx | gcon-vm before | after | after / iconx |
|---|---:|---:|---:|---:|
| initialisation | 320 | 245 | 202 | 0.63 |
| main pass | 2,710 | 2,400 | 1,265 | 0.47 |

Output is identical on `test.wav` and `test_big.wav`, also under the
address and undefined-behaviour sanitizers. What is left is spread over
the loop itself (operand fetch and dispatch, `operint`), the collector
(about a tenth: 23 collections as the heap grows to some 285 MB of
records, each tracing it all) and frame set-up in `newframe`.

## Tuning: string scanning, `find` and `upto` (2026-11-26)

The micro-benchmarks `"abcde" ? upto('d')` and `"abcde" ? find("de")`
were the nearest to iconx (0.96 and 0.95). Profiled, about two fifths
of the time was allocation and collection, and not chiefly the
generators': `?` allocated a scanning environment on entry and another
on each exchange.

- **Scanning environments in place.** A saved environment is held only
  in its scan's temporary (co-expressions copy locals, never
  temporaries), so `scanswap` exchanges it with the current one in
  place, and `scanenter` reuses the block the same scan's previous run
  left in its temporary (that run has ended when the scan is entered
  again). Entering a scan in a loop allocates nothing after the first
  time. gcon's Go VM avoids the allocation by packing the environment
  into the value; gcon-vm's 48-bit count has no room for a length and a
  position.
- **A replaced generator reused** (`newgen`): a call of a builtin
  generator overwrites its handle's slot, the only reference to the
  generator there, so the call passes that generator to the builtin
  (`vm.spare`), which reuses it if it is of the same kind (the same
  `next` function). `find` and `upto` use it. A generator that ends by
  failing has its handle cleared; keeping the last of those for reuse
  too, and using `newgen` for `to` and `!x`, made no measurable
  difference (`every` loops of them), so it was not kept.
- **`upto` on one-byte characters**: for an ASCII subject, or bytes
  against a cset of bytes, the test is a bit lookup in the loop rather
  than a call of `matchat` per character.

30 million `"abcde" ? upto('d')` went from 1.04 s to 0.47 s; without
the generator reuse, 0.64 s. `tests/bench.sh` after (ns per
iteration):

| benchmark | iconx | Go VM | gcon-vm | / iconx |
|---|---:|---:|---:|---:|
| empty loop | 7.6 | 6.9 | 2.9 | 0.38 |
| `*"abcde"` | 10.0 | 10.4 | 3.9 | 0.39 |
| `upto` | 40.3 | 49.6 | 17.0 | 0.42 |
| `find` | 39.6 | 45.3 | 21.8 | 0.55 |
| `x + 407` | 15.1 | 10.9 | 4.5 | 0.30 |
| `p()` | 15.7 | 24.9 | 9.6 | 0.61 |
| `q(1, 2)` | 31.6 | 46.7 | 14.1 | 0.45 |
| `L[3]` | 14.9 | 18.7 | 4.5 | 0.30 |
| `left("a", 10)` | 26.4 | 58.8 | 23.1 | 0.88 |
| `map(...)` | 27.6 | 44.5 | 23.5 | 0.85 |
| `move(1)` | 36.6 | 33.1 | 13.6 | 0.37 |
| table insert/delete | 37.3 | 99.0 | 20.9 | 0.56 |
| `pop(push(L, 0))` | 27.5 | 28.8 | 14.9 | 0.54 |
| same, empty list | 27.6 | 29.4 | 15.3 | 0.56 |
| `@C` | 2570 | 25.3 | 12.8 | 0.005 |
| `@(create 1)` | 17385 | 121.2 | 41.0 | 0.002 |

(These include the phwbi changes: `L[3]` and `x + 407` gained from the
inline `deref` and subscript fast path.) The geometric mean over the
first fourteen is now about 0.48 of iconx's time. `left` and `map` are
the nearest to parity.

## loadfunc (2026-11-26)

As planned (above): `loadfunc(file, name)` loads a C function written to
Icon 9.5's `icall.h`, and existing code recompiles unchanged. The IPL's
`ipl/cfuncs` (eleven files, 29 functions) build against gcon-vm's
`include/icall.h`, which is 9.5's file as it is (public domain), and
`tests/cfuncs.sh` runs them: the same library, loaded by iconx, gives
the same output except for an iconx bug (below). gcon resolves
`loadfunc` as a function it lacks; gcon-vm resolves any such reference
by name to its own functions first, so no format change was needed. On
gcon's Go VM it stays error 121. `function()` now generates `loadfunc` on
gcon-vm, as on iconx, so the two IPL programs that list the functions
(`gftrace`, `ifncsgen`) differ from the Go VM there by that one
function.

- **loadfunc** follows iconx's `fload.r`: the library's handle is kept
  for the next call with the same file name; `dlsym` is tried again with
  an underscore; a missing library or function writes iconx's
  diagnostic (`loadfunc("f","n"): ` and `dlerror()`) to `&errout` and
  raises error 216. The result is a function (`image` gives `function
  name`; `args` gives -1, as iconx's variadic built-ins), a new one each
  time.
- **Calls** convert the arguments to descriptors in iconx's layout
  (`icall.h`'s), with `argv[0]` &null, and treat the status as iconx's
  glue does: 0 returns `argv[0]`, -1 fails, n raises error n with
  `argv[0]` as the offending value unless it is &null. A result gcon-vm
  cannot represent is error 123.
- **Memory: an arena per call, results copied.** Everything made for a
  call (string copies, real, file, list and large-integer blocks, and
  what C allocates with `alcstr`, `alcreal`, `alcfile`) is in an arena
  freed when the call returns, after the result is converted and copied
  into the heap. C code never holds a pointer into gcon-vm's heap, so the
  collector cannot hurt it (iconx's hazard does not arise), and a
  string argument is a NUL-terminated copy, so `StringVal`'s read of the
  byte past the end is safe. A pointer C keeps from one call to the next
  is invalid, as it is in iconx after a collection.
- **Strings from C** take the unit their contents give them, as a
  literal's: ASCII, else text if valid UTF-8, else bytes (so a 9.5
  function giving Latin-1 behaves as in 9.5).
- **Large integers** are passed as iconx's bignum blocks (half-word
  digits, most significant first: gcon-vm's digits reversed), and
  converted back (`lgconv` works).
- **Values C cannot take apart** (csets, records, lists, sets, tables,
  procedures, co-expressions) are passed as blocks with iconx's type
  codes, so `IconType` is right, and convert back to the values they
  stand for. A list's block begins as iconx's list block, so `ListLen`
  reads its size, and `cpslots` (iconx's, which `ilists.c` uses) gives
  its elements.
- **Files** are lent as stdio streams on a duplicate of the descriptor,
  closed after the call, with iconx's status bits (read, write, pipe);
  gcon-vm's own buffered output is written first, and C's stdout and
  stderr are flushed after. Input gcon-vm has read ahead stays in its
  buffer, unseen by C (so `fpoll` cannot see it). A file C makes
  (`RetFile`, as `tconnect` does) becomes a gcon-vm file on its
  descriptor.
- **External values** are a new kind, `KExternal`, appended after the
  kinds gcon shares (it never appears in a built program). The block is
  iconx's (`alcexternal`: title, size, serial number from 1, function
  list, data), in gcon-vm's heap, which never moves it. `type`, `image`,
  `copy` and sorting call the block's `extlname`, `extlimage`,
  `extlcopy` and `extlcmp`, with iconx's defaults: "external";
  `name_id(words)`; the value itself; by name, then serial number.
  Externals sort after records, as iconx's. One difference: iconx's
  default image of an external with no data comes out garbled
  (`externalxstr_` for `external_1(0)` in `tests/cfuncs.sh`: it formats
  into a string it has just allocated, in a region the name's string
  overlaps); gcon-vm gives the intended `external_1(0)`.
- **Exporting the callbacks.** A library built as `icall.h`'s are
  leaves the callbacks unresolved (`mklib.sh`: on macOS `-bundle
  -undefined ...`, elsewhere `-shared`), so the executable exports them:
  they are marked used and visible, which keeps them through `-flto`, and
  `build.sh` links with `-rdynamic` on ELF systems (macOS exports an
  executable's symbols already). Tested on macOS only so far.
- **Not provided:** `internal.c`'s `peek` and `spy` read memory at
  iconx's addresses and mean nothing here (they load, and do what their
  code does with gcon-vm's descriptors). `cfunc.icn`'s `pathload` finds
  the library through `$FPATH`; iconx appends its own directory to
  `$FPATH`, so the library built with iconx is found by default. gcon
  does not yet ship the built library or `cfunc.icn`, so a program that
  `link cfunc`s needs both made (as `tests/cfuncs.sh` does) and `$FPATH`
  set.

## Graphics, first part (2026-11-26)

gcon's design puts graphics in a separate display server (gcon's
`docs/design.md`, "Graphics: a separate display server"); with the VM
now in C, gcon-vm takes the part that design gives "gconx": it owns
Icon's graphics semantics, and gcon-graphics (icon-sdl's SDL3 + Blend2D
backend) owns windows, canvases and drawing, behind the protocol in
gcon-graphics's `docs/protocol.md`. Of the reasons for the split, only
"gcon stays pure Go" has gone; gcon-vm still builds with nothing but a
C compiler (the server needs C++, CMake and SDL3), the macOS main-thread
rule stays in the server, a server crash cannot corrupt the
interpreter, and remote display stays possible.

- **`src/graphics.c`** is iconx's device-independent layer (fwindow.r,
  rwindow.r, rcolor.r) and icon-sdl's backend (rsdlwin.ri,
  rsdlrsc.ri), ported, with protocol requests where icon-sdl called its
  canvas and platform layers. Window states, contexts and bindings are
  iconx's (`wstate`, `wcontext`, `wbinding`), so `Clone` and `Couple`
  share canvases and contexts as there. A window is a `File` whose
  `gfx` is its binding (`type` "window", `image` "window_n:m(label)").
  Colours are allocated as icon-sdl's are, X's way (16-bit X colours
  remembered per device colour, the first name per X colour), so
  `Pixel` and `Fg` answer as the X backend does.
- **`src/gfxproto.c`** starts the server on the first graphics request
  (`$GCONGRAPHICS`, else gcon-graphics beside gcon-vm, else on `$PATH`),
  as a child on a socketpair, and buffers requests: they go when a reply
  is needed, when the program waits or flushes, and at exit, when
  closing the socket makes the server close the windows and exit.
- **gcs** are sent lazily: each context has a server gc, and a drawing
  request sends the gc first only if the drawing state differs from
  what was last sent; a call that draws with a variant (CopyArea's copy
  mode) uses a scratch gc.
- **Events** are fetched (`WAIT`), as the protocol has it: `Event`,
  `Pending` and `wattrib` ask, and while a program draws without
  pausing, every 64th drawing request checks whether 100 ms have passed
  since windows were last shown, and if so shows them and collects
  events (iconx's `pollevent`, which runs every few hundred
  instructions in any graphics build; here non-graphics programs pay
  nothing). Events go onto each window's Icon list in iconx's encoding
  (`qevent`), which programs see and may change through `Pending`.
- **Keywords.** gcon now compiles the graphics keywords as keyword
  operations; gcon's Go VM fails them, as an Icon without graphics
  does. Here `&x`, `&y`, `&row`, `&col` and `&interval` are variables
  (`VGraphics`) with iconx's side effects (`&x` sets `&col` ...), error
  140 before any event; `&window` accepts a window or &null.
- `open` takes attributes (its arity is -3 here, as in a graphics
  iconx), and `&features` now includes "dynamic loading", "external
  values" and "graphics".

**Done in this part:** open and close, the attributes (all of
`wattrib`'s, but `image=` and `iconimage=`, which wait for images),
colours (names, phrases, X names, hex, gamma), fonts and `TextWidth`,
lines, points, segments, rectangles, polygons, arcs and circles,
`DrawString`, `EraseArea`, `CopyArea`, `Pixel`, patterns, `Clip`,
`Clone`, `Couple`, `Uncouple`, `Event`, `Pending`, `WAttrib`, `WFlush`,
`WSync`, `Raise`, `Lower`, `Alert`, `GotoRC`, `GotoXY`, `QueryPointer`,
`ColorValue`, `WDefault`, and mutable colours as icon-sdl has them (not
supported). phwbi opens its window and lays out its vidgets.
**Still to do:** `DrawImage`, `DrawCurve`, the palette functions,
`ReadImage`/`WriteImage` (image files), `write` and `read` on a
window, the text cursor, icons.

**Tests.** `tests/graphics.sh` runs `tests/graphics.icn` on a hidden
canvas (no display needed): attributes, colours, fonts, drawing read
back with `Pixel`, patterns, `Clone`, the event list's encoding through
`Pending` and `Event`, keywords, errors. Against gcon's Go VM
(`tests/vs-go.sh`) three reference tests now differ, all through
`&features` (`env`, `features`, `prepro`: the last two also check that
each feature has a predefined preprocessor symbol, which gcon does not
yet define), and gcon's program test `keywords` expects the graphics
keywords to fail, as without graphics.

**A correction to the comparisons.** `tests/vs-go.sh` ran its
reference programs with `gcon run`, which, since gcon-vm was installed
in gcon's root, handed them to that gcon-vm rather than the Go VM; the
"identical to the Go VM" counts since then compared gcon-vm with its
installed self. It now sets `GCONVM=go`; rerun so, the reference tests
are identical but for the three above. In the IPL, 257 programs are
identical and four differ, all as expected: `envelope` (its pipe, as
before), `gftrace` and `ifncsgen` (`function()` now generates the
graphics functions, as a graphics iconx's does) and `irunerr` (which
lists the error texts, now with 140 to 147).

## Retiring gcon's Go VM, step 1: recorded runs (2026-11-26)

Sean's decision: gcon's Go VM is to be retired, gcon + gcon-vm being the
sole implementation, since keeping two implementations of the semantics
now forks behaviour at every new feature (`loadfunc`, graphics,
`&features`). The steps: (1) record the Go VM's runs as expected
results; (2) `gcon run` always uses gcon-vm, and gcon's tests that run
programs use it; (3) split gcon's `internal/vm` into the program format,
which the compiler needs, and the interpreter, which goes; (4) the
consequences: predefined symbols for gcon-vm's features, gcon's
`keywords` test, the READMEs and design notes.

Step 1: `tests/expected.sh` runs the reference tests and the IPL's
programs and compares each with its recorded run (`tests/expected/`),
taking the place of `tests/vs-go.sh`. Recorded from the Go VM, each
program run twice: 351 recorded, 23 skipped (11 not built, 9 too slow,
3 nondeterministic). Reading the recordings and the programs then
moved 33 more to skipped: three whose output carries the machine (host
name, user, environment: they must not be committed either) and 30
whose output depends on the clock (dates in their output, `randomize`,
`saytime`), which two runs a second apart cannot reveal. Four are
recorded from gcon-vm by decision (`tests/expected/ACCEPTED`: graphics
functions and errors, and `&features`). Result: 318 pass, 56 skipped.
