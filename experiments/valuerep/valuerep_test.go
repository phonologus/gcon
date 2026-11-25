// Package valuerep measures two representations of Icon values for the
// VM: the interface (Value = any) gcon has used, where an integer or real
// beyond the few Go preboxes allocates when stored, and a variable such
// as L[i] is a boxed struct; and a tagged struct of a kind, a number and
// a pointer, which allocates for neither. Both interpreters share one
// dispatch loop, frame and instruction layout, modelled on gcon's, and
// run the same three loops. It is an experiment, not part of gcon; see
// docs/design.md ("Decision: values as tagged structs").
//
//	go test -bench . -benchmem ./experiments/valuerep
package valuerep

import (
	"math"
	"testing"
	"unsafe"
)

// Instructions: three-address, operands are frame slots.
type op uint8

const (
	opAdd   op = iota // a := b + c (integer or real)
	opLE              // if b <= c goto next else goto l
	opIndex           // a := variable b[c]
	opDeref           // a := value of variable b
	opJump            // goto l
	opHalt
)

type instr struct {
	op      op
	a, b, c int32
	l       int32
}

// The programs, over slots: 0 i, 1 n, 2 x, 3 one, 4 k407, 5 L, 6 three, 7 v, 8 r, 9 k1.5.
var (
	intLoop = []instr{ // while i <= n do { x := i + 407; i := i + 1 }
		{op: opLE, b: 0, c: 1, l: 4},
		{op: opAdd, a: 2, b: 0, c: 4},
		{op: opAdd, a: 0, b: 0, c: 3},
		{op: opJump, l: 0},
		{op: opHalt},
	}
	indexLoop = []instr{ // while i <= n do { v := L[3]; i := i + 1 }
		{op: opLE, b: 0, c: 1, l: 5},
		{op: opIndex, a: 7, b: 5, c: 6},
		{op: opDeref, a: 7, b: 7},
		{op: opAdd, a: 0, b: 0, c: 3},
		{op: opJump, l: 0},
		{op: opHalt},
	}
	realLoop = []instr{ // while i <= n do { r := r + 1.5; i := i + 1 }
		{op: opLE, b: 0, c: 1, l: 4},
		{op: opAdd, a: 8, b: 8, c: 9},
		{op: opAdd, a: 0, b: 0, c: 3},
		{op: opJump, l: 0},
		{op: opHalt},
	}
)

// ---------------------------------------------------------------------
// Interface values, as gcon has them.

type iValue = any
type iInt int64
type iReal float64
type iList struct{ elems []iValue }
type iListElem struct {
	l *iList
	i int
}

func iRun(code []instr, f []iValue) {
	for pc := 0; ; {
		in := &code[pc]
		switch in.op {
		case opAdd:
			switch x := f[in.b].(type) {
			case iInt:
				y := f[in.c].(iInt)
				s := x + y
				if (s > x) != (y > 0) {
					panic("overflow")
				}
				f[in.a] = s
			case iReal:
				f[in.a] = x + f[in.c].(iReal)
			}
			pc++
		case opLE:
			if f[in.b].(iInt) <= f[in.c].(iInt) {
				pc++
			} else {
				pc = int(in.l)
			}
		case opIndex:
			l := f[in.b].(*iList)
			f[in.a] = iListElem{l, int(f[in.c].(iInt)) - 1}
			pc++
		case opDeref:
			if v, ok := f[in.b].(iListElem); ok {
				f[in.a] = v.l.elems[v.i]
			}
			pc++
		case opJump:
			pc = int(in.l)
		case opHalt:
			return
		}
	}
}

func iFrame(n int) []iValue {
	return []iValue{iInt(1), iInt(n), nil, iInt(1), iInt(407),
		&iList{[]iValue{iInt(10), iInt(20), iInt(30)}}, iInt(3), nil, iReal(0), iReal(1.5)}
}

// ---------------------------------------------------------------------
// Tagged struct values.

type kind uint8

const (
	kNull kind = iota
	kInt
	kReal
	kList
	kListElem // variable: p is the list, n the index from 0
)

type tValue struct {
	k kind
	n uint64
	p unsafe.Pointer
}

type tList struct{ elems []tValue }

func tInt(i int64) tValue      { return tValue{k: kInt, n: uint64(i)} }
func tReal(r float64) tValue   { return tValue{k: kReal, n: math.Float64bits(r)} }
func (v tValue) int() int64    { return int64(v.n) }
func (v tValue) real() float64 { return math.Float64frombits(v.n) }

func tRun(code []instr, f []tValue) {
	for pc := 0; ; {
		in := &code[pc]
		switch in.op {
		case opAdd:
			x, y := f[in.b], f[in.c]
			switch x.k {
			case kInt:
				s := x.int() + y.int()
				if (s > x.int()) != (y.int() > 0) {
					panic("overflow")
				}
				f[in.a] = tInt(s)
			case kReal:
				f[in.a] = tReal(x.real() + y.real())
			}
			pc++
		case opLE:
			if f[in.b].int() <= f[in.c].int() {
				pc++
			} else {
				pc = int(in.l)
			}
		case opIndex:
			f[in.a] = tValue{k: kListElem, n: uint64(f[in.c].int() - 1), p: f[in.b].p}
			pc++
		case opDeref:
			if v := f[in.b]; v.k == kListElem {
				f[in.a] = (*tList)(v.p).elems[v.n]
			}
			pc++
		case opJump:
			pc = int(in.l)
		case opHalt:
			return
		}
	}
}

func tFrame(n int) []tValue {
	l := &tList{[]tValue{tInt(10), tInt(20), tInt(30)}}
	return []tValue{tInt(1), tInt(int64(n)), {}, tInt(1), tInt(407),
		{k: kList, p: unsafe.Pointer(l)}, tInt(3), {}, tReal(0), tReal(1.5)}
}

// ---------------------------------------------------------------------

func TestSameResults(t *testing.T) {
	for _, code := range [][]instr{intLoop, indexLoop, realLoop} {
		fi, ft := iFrame(1000), tFrame(1000)
		iRun(code, fi)
		tRun(code, ft)
		if int64(fi[0].(iInt)) != ft[0].int() || float64(fi[8].(iReal)) != ft[8].real() {
			t.Fatalf("results differ: %v %v", fi, ft)
		}
	}
	fi, ft := iFrame(5), tFrame(5)
	iRun(indexLoop, fi)
	tRun(indexLoop, ft)
	if int64(fi[7].(iInt)) != 30 || ft[7].int() != 30 {
		t.Fatal("subscript")
	}
}

func bench(b *testing.B, code []instr, iface bool) {
	if iface {
		iRun(code, iFrame(b.N))
	} else {
		tRun(code, tFrame(b.N))
	}
}

func BenchmarkIntLoopInterface(b *testing.B)   { bench(b, intLoop, true) }
func BenchmarkIntLoopStruct(b *testing.B)      { bench(b, intLoop, false) }
func BenchmarkIndexLoopInterface(b *testing.B) { bench(b, indexLoop, true) }
func BenchmarkIndexLoopStruct(b *testing.B)    { bench(b, indexLoop, false) }
func BenchmarkRealLoopInterface(b *testing.B)  { bench(b, realLoop, true) }
func BenchmarkRealLoopStruct(b *testing.B)     { bench(b, realLoop, false) }

// ---------------------------------------------------------------------
// A procedure call with frames on a contiguous stack, as gcon-vm's
// experiments/spike measures it in C: while i <= n do { v := q(i, n);
// i := i + 1 }, where q(a, b) is local c; c := a; return b.

const (
	opCall op = opHalt + 1 + iota
	opReturn
	opMove
)

type retEntry struct {
	pc   int
	code []instr
	base int
	dst  int32
}

func tRunCalls(code, qcode []instr, stack []tValue) {
	pc, base, sp := 0, 0, 64
	var rs [1024]retEntry
	rsp := 0
	f := stack[base:]
	for {
		in := &code[pc]
		switch in.op {
		case opAdd:
			x, y := f[in.b], f[in.c]
			s := x.int() + y.int()
			if (s > x.int()) != (y.int() > 0) {
				panic("overflow")
			}
			f[in.a] = tInt(s)
			pc++
		case opLE:
			if f[in.b].int() <= f[in.c].int() {
				pc++
			} else {
				pc = int(in.l)
			}
		case opJump:
			pc = int(in.l)
		case opCall:
			nf := stack[sp:]
			nf[0], nf[1], nf[2] = f[in.b], f[in.b+1], tValue{}
			rs[rsp] = retEntry{pc + 1, code, base, in.a}
			rsp++
			base, sp = sp, sp+3
			f, code, pc = nf, qcode, 0
		case opMove:
			f[in.a] = f[in.b]
			pc++
		case opReturn:
			v := f[in.b]
			rsp--
			e := rs[rsp]
			sp, base = base, e.base
			f, code, pc = stack[base:], e.code, e.pc
			f[e.dst] = v
		case opHalt:
			return
		}
	}
}

func BenchmarkCallLoopStruct(b *testing.B) {
	code := []instr{
		{op: opLE, b: 0, c: 1, l: 4},
		{op: opCall, a: 7, b: 0},
		{op: opAdd, a: 0, b: 0, c: 3},
		{op: opJump, l: 0},
		{op: opHalt},
	}
	qcode := []instr{{op: opMove, a: 2, b: 0}, {op: opReturn, b: 1}}
	stack := make([]tValue, 1<<16)
	copy(stack, tFrame(b.N))
	tRunCalls(code, qcode, stack)
	if stack[0].int() != int64(b.N)+1 {
		b.Fatal("wrong count")
	}
}
