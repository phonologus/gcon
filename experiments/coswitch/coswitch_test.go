// Package coswitch measures candidate implementations of Icon
// co-expression switching in Go. It is an experiment, not part of gcon;
// see docs/design.md ("Co-expressions") for the results and the
// comparison with iconx 9.5 (coswitch.icn).
//
//	go test -bench . ./experiments/coswitch
package coswitch

import (
	"iter"
	"testing"
)

var sink int

// coexp is a symmetric co-expression: a goroutine with a private inbox.
// Transfer is "send to the target's inbox, then block on our own".
type coexp struct{ in chan int }

func newCo(body func(self *coexp)) *coexp {
	c := &coexp{in: make(chan int)}
	go func() { <-c.in; body(c) }()
	return c
}

// BenchmarkChanSwitch: one @c round trip, c = create |1.
func BenchmarkChanSwitch(b *testing.B) {
	main := &coexp{in: make(chan int)}
	c := newCo(func(self *coexp) {
		for {
			main.in <- 1
			<-self.in
		}
	})
	for i := 0; i < b.N; i++ {
		c.in <- 0
		sink += <-main.in
	}
}

// BenchmarkChanCreate: @(create 1). Each iteration leaks a parked
// goroutine, as a goroutine-based VM would without cleanup hooks.
func BenchmarkChanCreate(b *testing.B) {
	main := &coexp{in: make(chan int)}
	for i := 0; i < b.N; i++ {
		c := newCo(func(self *coexp) {
			main.in <- 1
			<-self.in
		})
		c.in <- 0
		sink += <-main.in
	}
}

// BenchmarkPullSwitch: asymmetric co-expression via iter.Pull, which uses
// the runtime's coroutine switch and bypasses the scheduler.
func BenchmarkPullSwitch(b *testing.B) {
	next, stop := iter.Pull(func(yield func(int) bool) {
		for yield(1) {
		}
	})
	defer stop()
	for i := 0; i < b.N; i++ {
		v, _ := next()
		sink += v
	}
}

func BenchmarkPullCreate(b *testing.B) {
	for i := 0; i < b.N; i++ {
		next, stop := iter.Pull(func(yield func(int) bool) { yield(1) })
		v, _ := next()
		sink += v
		stop()
	}
}

// Explicit-stack VM: a co-expression is its own frame stack and switching
// swaps the current-stack pointer. A toy lower bound only: the "run until
// suspend" step is a stand-in for the real dispatch loop.
type frame struct{ pc, val int }
type vmco struct{ stack []frame }
type vm struct{ cur *vmco }

func (m *vm) activate(c *vmco) int {
	prev := m.cur
	m.cur = c
	f := &c.stack[len(c.stack)-1]
	f.pc++
	v := f.val
	m.cur = prev
	return v
}

func BenchmarkVMSwitch(b *testing.B) {
	m := &vm{}
	c := &vmco{stack: []frame{{val: 1}}}
	for i := 0; i < b.N; i++ {
		sink += m.activate(c)
	}
}

func BenchmarkVMCreate(b *testing.B) {
	m := &vm{}
	for i := 0; i < b.N; i++ {
		c := &vmco{stack: make([]frame, 1, 8)}
		c.stack[0].val = 1
		sink += m.activate(c)
	}
}
