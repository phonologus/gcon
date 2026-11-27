# The instructions

gcon-vm runs a procedure's instructions one at a time, in a loop that
never calls itself: goal-directed evaluation is compiled to jumps (the
Proebsting translation, as Goaldi does), and a procedure call pushes a
heap-allocated frame instead of recursing. There are 20 instructions.
[The built program](program.md) gives their encoding; this page gives
what each does.

## Operands and labels

An instruction's operands name a **temporary** of the frame (written
`t2` in `gcon dis`'s listings), a **local** (`l0`; the parameters come
first), a **global** (`g1`; statics are globals too) or a **constant**
(`k0`). A temporary may hold a variable, so that an expression can
produce one (`x[i] := 1`). Where an instruction reads an operand's
value, a variable is dereferenced.

Most instructions that can fail have two labels: `l1`, where execution
continues if the instruction succeeds, and `l2`, where it continues if
it fails (written `->l1/l2`). Failure is never a separate state: an
expression that fails jumps to the code that resumes its last
generator, or to the end of its bounded expression.

Generators are resumed through a **handle**, the temporary `h`. A call
that can produce more results leaves in `h` what to resume (a suspended
procedure's frame, or a built-in generator); `resume` resumes it. A
handle that holds nothing makes `resume` fail.

## A listing

```icon
procedure main()
   every write(1 to 3)
end
```

Output:

```
1
2
3
```

`gcon dis` lists it so (the constants and globals first, then each
procedure; on each line the instruction's index, its source line, the
instruction):

```
procedure main (ex.icn)
  params 0; locals 0; temps 6
      0     1  goto ->1
      1     2  call t2 := k2 k0 k1 ->3/6 h=t3
      2     2  resume t2 := ->3/6 h=t3
      3     2  call t4 := g1 t2 ->4/2 h=t5
      4     2  resume t4 := ->4/2 h=t5
      5     2  goto ->6
      6     3  fail
```

Instruction 1 calls `k2`, the operator `to`, with 1 and 3: its first
result goes to `t2`, the generator to `t3`, and execution goes on at 3,
which calls `g1`, `write`. `write` produces one result, so `t5` is left
empty; `every` asks for another, so 4 resumes `t5`, which fails, to 2,
which resumes `to` in `t3` for the next number. When `to` is exhausted,
2 fails to 6, the end of `main`.

## The instructions

| Instruction | Written | Does |
|---|---|---|
| `goto` | `goto ->L` | continue at L |
| `igoto` | `igoto t` | continue at the label held in the temporary t (to resume whichever alternative produced the last result) |
| `setlabel` | `setlabel t := L` | put the label L in the temporary t, for `igoto` |
| `move` | `move t := x` | put the value of x in t |
| `movevar` | `movevar t := x` | put the variable x itself in t, so that t can be assigned to |
| `assign` | `assign d := x f ->L1/L2` | assign the value of x to the variable d with the assignment operator f (`:=`, `:=:` and the others); L1, or L2 if f fails |
| `oper` | `oper t := f a... ->L1/L2` | apply the operator f, which produces at most one result, to the arguments; its result to t and L1, or L2 if it fails |
| `call` | `call t := x a... ->L1/L2 h=th` | call the value of x with the arguments (below) |
| `resume` | `resume t := ->L1/L2 h=th` | resume what th holds: its next result to t and L1, or L2 |
| `return` | `return x` | return the value of x (or a variable, where a procedure may return one) to the caller |
| `fail` | `fail` | the procedure fails |
| `suspend` | `suspend x ->L` | produce the value of x to the caller, keeping the frame; resumed, continue at L |
| `once` | `once g ->L` | if the global g is null, set it to 1 and go on; else continue at L (an `initial` clause) |
| `isnull` | `isnull x ->L1/L2` | L1 if the value of x is `&null`, else L2 (the tests `/x` and `\x`) |
| `create` | `create t := ->L` | a co-expression that starts at L in this procedure, with a copy of its locals, to t |
| `activate` | `activate t := x a ->L1/L2` | activate the co-expression x, transmitting a (or `&null`); the value it produces to t and L1, or L2 if it has finished |
| `coret` | `coret x ->L` | in a co-expression: produce the value of x to its activator; activated again, continue at L |
| `cofail` | `cofail` | in a co-expression: it has finished, and fails to its activator |
| `scanenter` | `scanenter t := x` | begin scanning the string x: save `&subject` and `&pos` in t, and set them to x and 1 |
| `scanswap` | `scanswap t` | exchange `&subject` and `&pos` with those saved in t, on leaving or re-entering a scanning expression |

## call

What `call` does depends on what it calls:

- **A procedure:** a new frame, with the arguments as its parameters
  (padded with `&null`, or the extra ones dropped, or collected in a
  list for a procedure declared `p(a[])`). The callee's `return` puts
  its result in the caller's `t`, empties `h` and continues at L1; its
  `suspend` does the same but leaves its frame in `h`; its `fail`
  empties `h` and continues at L2.
- **A built-in function or operator:** if it is a generator, its first
  result goes to `t` and L1, and the generator to `h` (or L2, if it
  produces nothing); otherwise its result, or failure, at once.
- **A record constructor:** a new record, to `t`.
- **An integer** i: the i-th argument (from the end, if negative), as
  Icon's `i(e1, ..., en)`; L2 if there is none.
- **A string** naming a procedure, built-in or operator (string
  invocation); anything else is run-time error 106.

`call` with the built-in `!` (kind 3) applies a procedure to a list's
elements as arguments: `p ! L`.

## Errors

A run-time error in `oper`, `assign`, `call`, `resume`, `activate` or
`scanenter` that `&error` converts to failure continues at the
instruction's L2, as if it had failed. Any other error ends the program
with iconx's message and traceback.
