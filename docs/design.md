# gcon design notes

gcon runs Icon 9.5 programs on a virtual machine written in Go. It is one
binary: it translates Icon source (via the icontools parser) to gcon's own
bytecode and executes it. Like icontools, it uses only the Go standard
library and supports Go 1.24 and later.

Compatibility target: the *language* of Icon 9.5, checked against the
reference implementation. Compatibility with icont's ucode/icode files
is not a goal; gcon has its own bytecode.

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

## Verification

- Oracle: `icon/tests/general` has 99 `.icn`/`.std` pairs. gcon's
  conformance test runs each program and compares its output with the
  `.std` file. Programs that do not yet pass are listed explicitly, so
  progress shows up in the diff.
- New syntax and semantics facts go here, with the C source or test that
  shows them.

## Plan

1. **Front end.** Share the icontools lexer, parser and name resolution
   (see "Open questions").
2. **Vertical slice.** Integers, strings, locals and globals, procedures,
   `every`, `to`, `|`, `&`, `if`, `while`, `suspend`, `!` on strings and
   lists, `write`. Translation to IR, lowering to bytecode, and the VM loop.
3. **Co-expressions** in the slice: `create`, `@`, `^`, `&source`,
   `&current`. Confirm the switch cost from the real VM loop.
4. Grow the value types and built-ins, driven by the conformance suite.
5. `gcon build` producing a self-contained executable (the gcon binary
   with the bytecode appended), the counterpart of icont's output.

## Open questions

- **How gcon imports the parser.** The icontools front end is in
  `internal/` (`token`, `lexer`, `syntax`, plus `sema` and `builtin` for
  name resolution), which a separate module cannot import.
