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
	"os"
	"strconv"
	"sync"
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
	prog       *Program
	stackLimit int // evaluation stack capacity in descriptors (see push)
	globals    []Value
	cur        *Frame        // the running frame
	co         *Coexp        // the running co-expression
	main       *Coexp        // &main
	subject    *String       // &subject
	pos        int           // &pos
	out        *bufio.Writer // &output's writer
	errout     *bufio.Writer // &errout's writer
	input      *File         // &input
	output     *File         // &output
	errfile    *File         // &errout
	files      []*File       // files opened, to flush at exit
	// The output of a write call interrupted by an error, written after
	// the error message (see writeArgs).
	partial     []byte
	partialFile *File
	stdin       io.Reader // the streams the VM was given, for child processes
	stdout      io.Writer
	stderr      io.Writer
	// The streams as operating-system files, when they are (nil when
	// not): a child process is given these directly, as C's popen and
	// system give a child the parent's descriptors, so that its output
	// does not depend on gcon relaying it and survives gcon's exit.
	stdinFile, stdoutFile, stderrFile *os.File
	serials                           map[string]int // last serial number issued, by type

	// Error conversion (errors.go): &error, and the last error converted.
	errorKw    Int
	errNum     int
	errVal     Value
	haveErrVal bool

	random      Int // &random (random.go)
	lastCharPos int // position of randomChar's last choice
	trace       Int // &trace (trace.go)
	level       int // &level
	// keywords.go
	dump                               Int
	collections                        [4]Int // total, static, string, block
	progname                           *String
	eventcode, eventsource, eventvalue Value

	// Deadline, if set, stops the program with status DeadlineStatus
	// once passed (checked every few thousand instructions). For tests.
	Deadline time.Time
	steps    uint32
}

// DeadlineStatus is the exit status of a program stopped by Deadline.
const DeadlineStatus = -1

// lockedWriter serializes writes: child processes (pipes, system)
// write to the VM's output streams from other goroutines, and standard
// output and error may be the same writer.
type lockedWriter struct {
	mu *sync.Mutex
	w  io.Writer
}

func (l lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}

// New returns a VM for prog with the given standard streams.
func New(prog *Program, stdin io.Reader, stdout, stderr io.Writer) *VM {
	inF, _ := stdin.(*os.File)
	outF, _ := stdout.(*os.File)
	errF, _ := stderr.(*os.File)
	mu := new(sync.Mutex)
	stdout, stderr = lockedWriter{mu, stdout}, lockedWriter{mu, stderr}
	g := make([]Value, len(prog.Globals))
	copy(g, prog.Globals)
	vm := &VM{
		prog:       prog,
		globals:    g,
		stackLimit: stackLimit(),
		out:        bufio.NewWriter(stdout),
		errout:     bufio.NewWriter(stderr),
		serials:    map[string]int{},
		subject:    Text(""),
		pos:        1,
		stdin:      stdin,
		stdout:     stdout,
		stderr:     stderr,
		stdinFile:  inF,
		stdoutFile: outF,
		stderrFile: errF,
	}
	vm.progname = Text("")
	vm.eventcode, vm.eventsource, vm.eventvalue = NullValue, NullValue, NullValue
	vm.input = &File{name: "&input", keyword: true, r: bufio.NewReader(stdin)}
	vm.output = &File{name: "&output", keyword: true, w: vm.out}
	vm.errfile = &File{name: "&errout", keyword: true, w: vm.errout, errout: true}
	return vm
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
		// As C's exit does for iconx, flush every open file, and close
		// the pipes written to, so that their commands see the end of
		// their input and finish. gcon then waits for a command only
		// if it must relay the command's output (when its own output
		// is not a file, as in tests); iconx does not wait. gcon's own
		// output goes first, so that a command's cannot overtake it.
		vm.out.Flush()
		for _, f := range vm.files {
			if f.w == nil || f.closed {
				continue
			}
			f.w.Flush()
			if f.cmd != nil {
				f.closed = true
				f.closer.Close()
				if vm.stdoutFile == nil || vm.stderrFile == nil {
					f.cmd.Wait()
				}
			}
		}
	}()
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
			vm.errout.Flush()
			if vm.partialFile != nil && vm.partialFile.w != nil {
				vm.partialFile.w.Write(vm.partial)
			}
			status = 1
		default:
			panic(e)
		}
		if vm.co != nil {
			vm.termDump() // &dump, as iconx's c_exit
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
	vm.serials["co-expression"] = 1
	vm.main = &Coexp{serial: 1, proc: main, state: coRunning, results: 1}
	vm.main.activator = vm.main
	vm.co = vm.main
	// As iconx, &trace starts at the value of the environment variable
	// TRACE, so the call of main can be traced.
	if t, err := strconv.ParseInt(os.Getenv("TRACE"), 10, 64); err == nil {
		vm.trace = Int(t)
	}
	mf := vm.newFrame(main, margs)
	if vm.tracing() {
		vm.traceCall(nil, 0, main, mf.locals[:main.NParams])
	}
	vm.level = 1
	vm.run(mf)
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

// assign assigns v to the variable o, reporting whether the assignment
// succeeded (assigning an invalid position to &pos fails).
func (vm *VM) assign(f *Frame, o Operand, v Value) bool {
	switch o.Kind {
	case LocalOp:
		f.locals[o.Index] = v
		return true
	case GlobalOp:
		vm.globals[o.Index] = v
		return true
	case TempOp:
		switch x := f.temps[o.Index].(type) {
		case *Value:
			*x = v
			return true
		case Var:
			return x.Set(v)
		}
	}
	Runerr(111, Deref(vm.get(f, o)))
	return false
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
// A run-time error that &error converts to failure resumes execution at
// the failing instruction's failure label.
func (vm *VM) run(f *Frame) {
	for !vm.exec(f) {
		f = vm.cur
	}
}

// exec executes from frame f until the program returns or fails, and
// reports true; it reports false when a run-time error has been
// converted to failure.
func (vm *VM) exec(f *Frame) (done bool) {
	defer func() {
		if done {
			return
		}
		r := recover()
		if e, ok := r.(*RuntimeError); !ok || !vm.convert(e) {
			panic(r)
		}
	}()
	vm.loop(f)
	return true
}

// loop is the interpreter's main loop.
func (vm *VM) loop(f *Frame) {
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
			if in.Fn != nil && (in.Fn.Name == ":=:" || in.Fn.Name == "<->") {
				// Both operands must be variables before either is
				// assigned (iconx's swap and rswap).
				for _, o := range []Operand{in.Dst, in.X} {
					if !vm.isVar(f, o) {
						Runerr(111, Deref(vm.get(f, o)))
					}
				}
			}
			if len(in.Args) == 1 {
				// One assignment of :=: or <->, naming the other operand:
				// if both are substrings of one string, the other one,
				// when to the right, shifts by the change in length, as
				// iconx's swap and rswap adjust it.
				d, ok1 := vm.ref(f, in.Dst).(*SubstrVar)
				o, ok2 := vm.ref(f, in.Args[0]).(*SubstrVar)
				if ok1 && ok2 && d.v == o.v {
					n := d.n
					if !vm.assign(f, in.Dst, Deref(vm.get(f, in.X))) {
						f.pc = int(in.L2)
						continue
					}
					if o.pos > d.pos {
						o.pos += d.n - n
					}
					f.pc = int(in.L1)
					continue
				}
			}
			if vm.assign(f, in.Dst, Deref(vm.get(f, in.X))) {
				f.pc = int(in.L1)
			} else {
				f.pc = int(in.L2)
			}
		case OpScanEnter:
			f.temps[in.Dst.Index] = vm.scanEnter(Deref(vm.get(f, in.X)))
			f.pc++
		case OpScanSwap:
			vm.scanSwap(f.temps[in.X.Index].(*scanEnv))
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
			args := vm.args(f, in.Args)
			if in.Fn.RefFirst {
				args[0] = vm.ref(f, in.Args[0])
			}
			if v, ok := in.Fn.Fn(vm, args); ok {
				f.temps[in.Dst.Index] = v
				f.pc = int(in.L1)
			} else {
				f.pc = int(in.L2)
			}
		case OpCall:
			fv := Deref(vm.get(f, in.X))
			args := vm.args(f, in.Args)
			if in.Fn == ApplyOp {
				args = applyArgs(args[0])
			}
			switch fv.(type) {
			case *Proc, *Builtin, *RecordType, Int:
			default:
				fv = vm.callable(fv, len(args))
				if b, ok := fv.(*Builtin); ok && b.NoDeref {
					for i, o := range in.Args {
						args[i] = vm.ref(f, o)
					}
				}
			}
			switch p := fv.(type) {
			case *Proc:
				vm.push(p)
				nf := vm.newFrame(p, args)
				if vm.tracing() {
					// The arguments as the procedure receives them, as
					// iconx traces them: padded with &null, extras
					// dropped or gathered into a list.
					vm.traceCall(f, f.pc, p, nf.locals[:p.NParams])
				}
				vm.level++
				nf.caller = f
				f = nf
				vm.cur = f
			case *Builtin:
				if p.RefFirst && len(args) > 0 && in.Fn != ApplyOp {
					args[0] = vm.ref(f, in.Args[0])
				}
				if in.X.Kind != ConstOp {
					// A call written in the program (the compiler's own
					// operator calls have constant callees).
					args = adjustArgs(p, args)
				}
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
			case *RecordType:
				f.temps[in.H] = nil
				f.temps[in.Dst.Index] = p.construct(args)
				f.pc = int(in.L1)
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
				vm.push(h.proc)
				if vm.tracing() {
					vm.traceAt(f, f.pc, h.proc.Name+" resumed")
				}
				vm.level++
				h.caller = f
				f = h
				vm.cur = f
			case Generator:
				vm.next(f, in, h)
			default:
				panic("vm: bad handle")
			}
		case OpReturn:
			v := vm.retDeref(f, vm.ref(f, in.X))
			vm.level--
			vm.pop(f)
			if vm.tracing() {
				vm.traceAt(f, f.pc, f.proc.Name+" returned "+vm.OutImage(v, 0))
			}
			c := f.caller
			if c == nil {
				if vm.co != vm.main {
					f = vm.cofail() // not generated: the compiler rejects this, as icont does
					continue
				}
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.Dst.Index] = v
			c.temps[ci.H] = nil
			c.pc = int(ci.L1)
			f = c
			vm.cur = f
		case OpFail:
			vm.level--
			vm.pop(f)
			if vm.tracing() {
				vm.traceAt(f, f.pc, f.proc.Name+" failed")
			}
			c := f.caller
			if c == nil {
				if vm.co != vm.main {
					f = vm.cofail()
					continue
				}
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.H] = nil
			c.pc = int(ci.L2)
			f = c
			vm.cur = f
		case OpSuspend:
			v := vm.retDeref(f, vm.ref(f, in.X))
			vm.level--
			vm.pop(f)
			if vm.tracing() {
				vm.traceAt(f, f.pc, f.proc.Name+" suspended "+vm.OutImage(v, 0))
			}
			f.pc = int(in.L1)
			c := f.caller
			if c == nil {
				if vm.co != vm.main {
					f = vm.cofail()
					continue
				}
				return
			}
			ci := &c.proc.Code[c.pc]
			c.temps[ci.Dst.Index] = v
			c.temps[ci.H] = f
			c.pc = int(ci.L1)
			f.caller = nil
			f = c
			vm.cur = f
		case OpCreate:
			c := vm.newCoexp(f.proc, int(in.L1), append([]Value(nil), f.locals...))
			c.from = vm.calledFrom(f)
			f.temps[in.Dst.Index] = c
			f.pc++
		case OpActivate:
			v := NullValue
			if len(in.Args) > 0 {
				v = Deref(vm.get(f, in.Args[0]))
			}
			f = vm.activate(f, in, Deref(vm.get(f, in.X)), v)
		case OpCoRet:
			f = vm.coret(f, Deref(vm.get(f, in.X)), int(in.L1))
		case OpCoFail:
			f = vm.cofail()
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
	Fatal    bool // never converted to failure (stack overflow)
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
		fmt.Fprintf(w, "File %s; Line %d\n", f.proc.FileAt(f.pc), f.proc.Code[f.pc].Line)
	}
	fmt.Fprintln(w, errorText[e.Num])
	if e.HasValue {
		fmt.Fprintf(w, "offending value: %s\n", vm.OutImage(e.Value, 0))
	}
	vm.traceback(w)
}

// retDeref prepares v, the operand of a return or suspend in frame f: as
// iconx's retderef does, a variable in f itself (a local or parameter)
// is dereferenced, since the frame is going away, but other variables
// (globals, statics, elements of structures, keywords) are produced as
// variables, so that f(x) := y assigns to what f returned.
func (vm *VM) retDeref(f *Frame, v Value) Value {
	if p, ok := v.(*Value); ok {
		for i := range f.locals {
			if p == &f.locals[i] {
				return *p
			}
		}
	}
	return v
}

// isVar reports whether operand o of frame f is a variable.
func (vm *VM) isVar(f *Frame, o Operand) bool {
	switch o.Kind {
	case LocalOp, GlobalOp:
		return true
	case TempOp:
		switch f.temps[o.Index].(type) {
		case *Value, Var:
			return true
		}
	}
	return false
}

// The evaluation stack. gcon's frames are on the heap, so recursion is
// limited only by memory; to stop runaway recursion as iconx does, with
// run-time error 301, each co-expression counts the stack its active
// calls would take in iconx: a frame costs 9 descriptors plus one for
// each parameter and local, which gives exactly the depths at which
// iconx overflows (55554 calls of a procedure with no parameters or
// locals, 45454 with one of each, 29412 with three and five). The
// capacity is 500000 descriptors, as measured, or MSTKSIZE words at two
// words a descriptor, as iconx reads MSTKSIZE. A suspended frame is not
// counted (as it is not in &level), nor is the base frame of main or of
// a co-expression.

// DefaultStackLimit is the evaluation stack's capacity in descriptors.
const DefaultStackLimit = 500000

func frameCost(p *Proc) int { return 9 + p.NLocals }

// push counts a call of p, failing with error 301 if the stack would
// overflow.
func (vm *VM) push(p *Proc) {
	c := frameCost(p)
	if vm.co.stack+c > vm.stackLimit {
		panic(&RuntimeError{Num: 301, Fatal: true})
	}
	vm.co.stack += c
}

// pop uncounts the active frame f as it returns, fails or suspends, if
// a call or resumption counted it (it has a caller).
func (vm *VM) pop(f *Frame) {
	if f.caller != nil {
		vm.co.stack -= frameCost(f.proc)
	}
}

// stackLimit returns the capacity from MSTKSIZE, if set to a positive
// number of words.
func stackLimit() int {
	if n, err := strconv.Atoi(os.Getenv("MSTKSIZE")); err == nil && n > 1 {
		return n / 2
	}
	return DefaultStackLimit
}
