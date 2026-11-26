# Tasks and channels on co-expressions

A proof of concept: Go's goroutines and channels as a small library in
plain Icon 9.5 (`chan.icn`, about 200 lines), built only on
co-expressions. Its tasks are cooperative and run on one thread, like Go
with `GOMAXPROCS=1`. Because co-expression switches are cheap in gcon,
programs built from many tasks run at speeds close to compiled Go's.

```icon
link chan

procedure producer(c, n)
   every send(c, 1 to n)
   close(c)
end

procedure main()
   c := chan()
   go(create producer(c, 5))
   while write(recv(c))          # Go's "for x := range c"
end
```

| | |
|---|---|
| `go(create e)` | start a task; main is a task too |
| `chan()`, `chan(n)` | an unbuffered channel, or one buffering n values |
| `send(c, x)` | send; blocks until received (or buffered) |
| `recv(c)` | receive; **fails** once `c` is closed and empty |
| `close(c)` | no more sends (on a file: the built-in `close`) |
| `items(c)` | generates what is received until close: `every x := items(c)` |
| `select(c1, c2, ...)` | receive from the first ready; `chan_which` is the channel |
| `yield()` | let the other ready tasks run |

`recv` failing on a closed channel fits Icon: Go's range loop becomes
`while x := recv(c)`, and `items(c)` is just `|recv(c)`. As in Go, the
program ends when main does, and when every task is blocked the
scheduler stops with `chan: all tasks are asleep - deadlock!`.

How it works: the scheduler is a co-expression looping over a queue of
ready tasks, activating each in turn. A task that must wait puts itself
on the channel's queue of senders or receivers and transfers to the
scheduler (`chan_PARK @ chan_sched`); whoever completes the exchange puts
it back on the ready queue. A blocking send and receive is a handful
of procedure calls and two or three co-expression transfers.

## Files

- `chan.icn`: the library.
- `chantest.icn`: every feature, including deadlock detection;
  `chantest.std` is its output, identical on iconx, gcon's Go VM and
  gcon-vm.
- `ring.icn`: a token passed m times round a ring of n tasks (n × m
  blocking hand-offs).
- `sieve.icn`: the concurrent prime sieve of Newsqueak and Go: one task
  per prime found.
- `go/`: the same ring and sieve in Go, for comparison.
- `bench.sh`: ring and sieve on gcon-vm, gcon's Go VM and iconx.

## Results (2026-11-26, Apple M-series)

Milliseconds (ns per hand-off for the ring), with peak memory:

| | gcon-vm | gcon Go VM | iconx 9.5 | Go, 1 proc | Go, all procs |
|---|---:|---:|---:|---:|---:|
| ring 1000 × 1000 | 205 ns, 22 MB | 2,376 ns | 4,760 ns | 90 ns | 120 ns |
| ring 10 × 100,000 | 219 ns, 19 MB | 1,040 ns | 4,920 ns | 69 ns | 102 ns |
| ring 100,000 × 10 | 280 ns, 504 MB | 2,835 ns, 770 MB | fails | 187 ns | 283 ns, 292 MB |
| sieve 1000 | 170 ms | 1,533 ms | 2,650 ms | 62 ms | 38 ms |
| sieve 3000 | 1,559 ms | 15,313 ms | 22,990 ms | 329 ms | 283 ms |

- **gcon-vm against iconx: 15 to 23 times faster.** It also scales:
  100,000 tasks take half a gigabyte (about 5 KB a task, the collector's
  headroom included; Go needs about 3 KB per goroutine) and the cost per
  hand-off hardly moves.
- **Against compiled Go:** the ring is 2 to 3 times Go's single-threaded
  time, and level with Go on all cores at 100,000 tasks. The sieve is
  about 5 times Go's time, because its filters do interpreted work
  (`%`, the calls into `chan.icn`, the queues) besides switching.
- **iconx** on this platform implements co-expressions with threads
  (`cannot create thread` at 100,000 tasks), so every transfer is a
  thread hand-off. Its collector also crashes (bus error) once a few
  hundred co-expressions have run for a while, so `bench.sh` gives it
  regions big enough never to collect.

## Limits

Cooperative scheduling, as Go's was before 1.14 without preemption: a
task that never sends, receives or yields keeps the others from running.
There is no parallelism, `select` only receives (no send cases and no
`default`), and there are no timers. All of these could be added in Icon
except parallelism.
