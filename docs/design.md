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
