# Performance

These are micro-benchmarks from `vm/tests/bench.sh`, which builds each
loop with both systems and reports wall-clock nanoseconds per
iteration, best of three. They were run on an Apple M5 with Apple clang
21 on 2026-11-26.

| benchmark | iconx (ns) | Gcon (ns) | Gcon / iconx |
|---|---:|---:|---:|
| empty loop | 7.4 | 3.8 | 0.52 |
| `*s` | 9.7 | 3.8 | 0.39 |
| `x + 407` | 14.8 | 4.2 | 0.28 |
| `L[3]` | 14.6 | 4.3 | 0.29 |
| `p()` | 15.5 | 9.4 | 0.61 |
| `q(1, 2)` | 30.6 | 13.7 | 0.45 |
| `s ? upto(c)` | 39.7 | 16.3 | 0.41 |
| `s ? find(s2)` | 38.5 | 21.2 | 0.55 |
| `move(1)` | 35.1 | 13.3 | 0.38 |
| `left("a", 10)` | 25.5 | 22.9 | 0.90 |
| `map(...)` | 27.3 | 23.0 | 0.84 |
| table insert and delete | 36.9 | 19.8 | 0.54 |
| `pop(push(L, 0))` | 27.1 | 14.3 | 0.53 |
| `@C`, a co-expression switch | 2,538 | 12.5 | 0.005 |
| `@(create 1)` | 17,636 | 41.5 | 0.002 |

Leaving out the co-expressions, the geometric mean is **0.49** of
iconx's time. iconx gives each co-expression its own C stack and
switches between them in assembly. Gcon's VM is not recursive, so a
co-expression is just a VM state, and a switch costs about as much as a
procedure call.

On a whole program, a signal-processing application (records in a big
list, field access in a tight loop) runs its main pass over 730,000
samples in 1,265 ms, against iconx's 2,710 ms (0.47). A built "hello,
world" starts in 6.6 ms, against iconx's 11.2.

