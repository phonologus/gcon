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
