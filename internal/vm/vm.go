// Package vm is gcon's virtual machine.
//
// Icon control never recurses on the Go stack (docs/design.md, "Decision:
// a non-recursive VM"). Goal-directed evaluation within a procedure is
// compiled to jumps; procedure call, return, suspension and resumption
// switch between heap-allocated frames. A suspended procedure is simply a
// frame whose pc is its resumption point, held in a temporary of the
// frame that called it.
package vm

import (
	"bufio"
	"fmt"
	"io"
	"time"
)

// Frame is the activation of a procedure.
type Frame struct {
	proc   *Proc
	locals []Value
	temps  []Value
	// pc is the next instruction. While the frame waits on a call or a
	// resumption, pc is that OpCall or OpResume; while the frame is
	// suspended, pc is its resumption point.
	pc     int
	caller *Frame
}

// VM executes a compiled program.
type VM struct {
	prog    *Program
	globals []Value
	cur     *Frame // the running frame
	out     *bufio.Writer
	errout  *bufio.Writer
	serials map[string]int // last serial number issued, by type

	// Deadline, if set, stops the program with status DeadlineStatus
	// once passed (checked every few thousand instructions). For tests.
	Deadline time.Time
	steps    uint32
}

// DeadlineStatus is the exit status of a program stopped by Deadline.
const DeadlineStatus = -1

// New returns a VM for prog writing to stdout and stderr.
func New(prog *Program, stdout, stderr io.Writer) *VM {
	g := make([]Value, len(prog.Globals))
	copy(g, prog.Globals)
	return &VM{
		prog:    prog,
		globals: g,
		out:     bufio.NewWriter(stdout),
		errout:  bufio.NewWriter(stderr),
		serials: map[string]int{},
	}
}

// exitSignal unwinds the VM for exit() and stop().
type exitSignal struct{ code int }

// deadlineSignal unwinds the VM when its Deadline has passed.
type deadlineSignal struct{}

// Run runs the program's main procedure with the given arguments and
// returns the exit status.
func (vm *VM) Run(args []string) (status int) {
	defer vm.out.Flush()
	defer vm.errout.Flush()
	defer func() {
		switch e := recover().(type) {
		case nil:
		case exitSignal:
			status = e.code
		case deadlineSignal:
			vm.out.Flush()
			fmt.Fprintln(vm.errout, "gcon: deadline exceeded")
			status = DeadlineStatus
		case *RuntimeError:
			vm.out.Flush()
			vm.report(e)
			status = 1
		default:
			panic(e)
		}
	}()
	var main *Proc
	if vm.prog.Main >= 0 {
		main, _ = vm.globals[vm.prog.Main].(*Proc)
	}
	if main == nil {
		Runerr(117)
	}
	var margs []Value
	if main.NParams > 0 {
		l := vm.NewList(nil)
		for _, a := range args {
			l.Elems = append(l.Elems, Text(a))
		}
		margs = []Value{l}
	}
	vm.run(vm.newFrame(main, margs))
	return 0
}

func (vm *VM) newFrame(p *Proc, args []Value) *Frame {
	f := &Frame{proc: p, locals: make([]Value, p.NLocals), temps: make([]Value, p.NTemps)}
	for i := range f.locals {
		f.locals[i] = NullValue
	}
	n := p.NParams
	if p.Variadic {
		n--
		rest := vm.NewList(nil)
		if len(args) > n {
			rest.Elems = append(rest.Elems, args[n:]...)
		}
		f.locals[n] = rest
	}
	for i := 0; i < n && i < len(args); i++ {
		f.locals[i] = args[i]
	}
	return f
}

// get returns operand o as stored: a temporary may hold a variable.
func (vm *VM) get(f *Frame, o Operand) Value {
	switch o.Kind {
	case TempOp:
		return f.temps[o.Index]
	case LocalOp:
		return f.locals[o.Index]
	case GlobalOp:
		return vm.globals[o.Index]
	case ConstOp:
		return vm.prog.Consts[o.Index]
	}
	panic("vm: bad operand")
}

// ref returns operand o as a variable where it is one.
func (vm *VM) ref(f *Frame, o Operand) Value {
	switch o.Kind {
	case LocalOp:
		return &f.locals[o.Index]
	case GlobalOp:
		return &vm.globals[o.Index]
	}
	return vm.get(f, o)
}

func (vm *VM) assign(f *Frame, o Operand, v Value) {
	switch o.Kind {
	case LocalOp:
		f.locals[o.Index] = v
		return
	case GlobalOp:
		vm.globals[o.Index] = v
		return
	case TempOp:
		switch x := f.temps[o.Index].(type) {
		case *Value:
			*x = v
			return
		case Var:
			x.Set(v)
			return
		}
	}
	Runerr(111, Deref(vm.get(f, o)))
}

// args dereferences the argument operands.
func (vm *VM) args(f *Frame, ops []Operand) []Value {
	if len(ops) == 0 {
		return nil
	}
	a := make([]Value, len(ops))
	for i, o := range ops {
		a[i] = Deref(vm.get(f, o))
	}
	return a
}

// run executes until the frame f, and so the program, returns or fails.
func (vm *VM) run(f *Frame) {
	vm.cur = f
	for {
		if vm.steps++; vm.steps&0xffff == 0 && !vm.Deadline.IsZero() && time.Now().After(vm.Deadline) {
			panic(deadlineSignal{})
		}
		in := &f.proc.Code[f.pc]
		switch in.Op {
		case OpGoto:
			f.pc = int(in.L1)
		case OpIGoto:
			f.pc = int(f.temps[in.X.Index].(Int))
		case OpSetLabel:
			f.temps[in.Dst.Index] = Int(in.L1)
			f.pc++
		case OpMove:
			f.temps[in.Dst.Index] = Deref(vm.get(f, in.X))
			f.pc++
		case OpMoveVar:
			f.temps[in.Dst.Index] = vm.ref(f, in.X)
			f.pc++
		case OpAssign:
			vm.assign(f, in.Dst, Deref(vm.get(f, in.X)))
			f.pc++
		case OpIsNull:
			if _, ok := Deref(vm.get(f, in.X)).(Null); ok {
				f.pc = int(in.L1)
			} else {
				f.pc = int(in.L2)
			}
		case OpOnce:
			if _, ok := vm.globals[in.X.Index].(Null); ok {
				vm.globals[in.X.Index] = Int(1)
				f.pc++
			} else {
				f.pc = int(in.L1)
			}
		case OpOper:
			if v, ok := in.Fn.Fn(vm, vm.args(f, in.Args)); ok {
				f.temps[in.Dst.Index] = v
				f.pc = int(in.L1)
			} else {
				f.pc = int(in.L2)
			}
		case OpCall:
			fv := Deref(vm.get(f, in.X))
			args := vm.args(f, in.Args)
			switch p := fv.(type) {
			case *Proc:
				nf := vm.newFrame(p, args)
				nf.caller = f
				f = nf
				vm.cur = f
			case *Builtin:
				if p.Gen != nil {
					g := p.Gen(vm, args)
					vm.next(f, in, g)
				} else {
					f.temps[in.H] = nil
					if v, ok := p.Fn(vm, args); ok {
						f.temps[in.Dst.Index] = v
						f.pc = int(in.L1)
					} else {
						f.pc = int(in.L2)
					}
				}
			case Int:
				// Integer invocation i(e1, ..., en) produces ei.
				f.temps[in.H] = nil
				if i := int(p); i >= 1 && i <= len(args) {
					f.temps[in.Dst.Index] = args[i-1]
					f.pc = int(in.L1)
				} else if i < 0 && -i <= len(args) {
					f.temps[in.Dst.Index] = args[len(args)+i]
					f.pc = int(in.L1)
				} else {
					f.pc = int(in.L2)
				}
			default:
				Runerr(106, fv)
			}
		case OpResume:
			switch h := f.temps[in.H].(type) {
			case nil:
				f.pc = int(in.L2)
			case *Frame:
				h.caller = f
				f = h
				vm.cur = f
			case Generator:
				vm.next(f, in, h)
			default:
				panic("vm: bad handle")
			}
		case OpReturn:
			v := Deref(vm.get(f, in.X))
			c := f.caller
			if c == nil {
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.Dst.Index] = v
			c.temps[ci.H] = nil
			c.pc = int(ci.L1)
			f = c
			vm.cur = f
		case OpFail:
			c := f.caller
			if c == nil {
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.H] = nil
			c.pc = int(ci.L2)
			f = c
			vm.cur = f
		case OpSuspend:
			v := Deref(vm.get(f, in.X))
			f.pc = int(in.L1)
			c := f.caller
			if c == nil {
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.Dst.Index] = v
			c.temps[ci.H] = f
			c.pc = int(ci.L1)
			f.caller = nil
			f = c
			vm.cur = f
		default:
			panic(fmt.Sprintf("vm: bad opcode %d", in.Op))
		}
	}
}

// next produces the next result of generator g for the OpCall or
// OpResume in.
func (vm *VM) next(f *Frame, in *Instr, g Generator) {
	if v, ok := g.Next(vm); ok {
		f.temps[in.H] = g
		f.temps[in.Dst.Index] = v
		f.pc = int(in.L1)
	} else {
		f.temps[in.H] = nil
		f.pc = int(in.L2)
	}
}

// Exit ends the program with the given status.
func (vm *VM) Exit(code int) { panic(exitSignal{code}) }

// Out returns the standard output writer.
func (vm *VM) Out() *bufio.Writer { return vm.out }

// ErrOut returns the standard error writer.
func (vm *VM) ErrOut() *bufio.Writer { return vm.errout }

// NewList returns a new list with the next list serial number.
func (vm *VM) NewList(elems []Value) *List {
	vm.serials["list"]++
	return &List{Elems: elems, serial: vm.serials["list"]}
}

// RuntimeError is an Icon run-time error.
type RuntimeError struct {
	Num      int
	Value    Value
	HasValue bool
}

func (e *RuntimeError) Error() string { return fmt.Sprintf("run-time error %d", e.Num) }

// Runerr raises run-time error num, with an offending value if given.
func Runerr(num int, v ...Value) {
	e := &RuntimeError{Num: num}
	if len(v) > 0 {
		e.Value, e.HasValue = v[0], true
	}
	panic(e)
}

// report writes a run-time error message as iconx does.
//
// TODO: the traceback.
func (vm *VM) report(e *RuntimeError) {
	w := vm.errout
	fmt.Fprintf(w, "\nRun-time error %d\n", e.Num)
	if f := vm.cur; f != nil && f.pc < len(f.proc.Code) {
		fmt.Fprintf(w, "File %s; Line %d\n", f.proc.File, f.proc.Code[f.pc].Line)
	}
	fmt.Fprintln(w, errorText[e.Num])
	if e.HasValue {
		fmt.Fprintf(w, "offending value: %s\n", vm.Image(e.Value))
	}
}
