package vm

// Co-expressions (docs/design.md, "Decision: a non-recursive VM").
//
// A co-expression is a chain of frames, like the main program, and
// switching between co-expressions is switching which chain the
// interpreter runs. A co-expression that is not running is in one of
// three states: not yet started; suspended after producing a result
// (its frame's pc is where to resume); or waiting at an activation of
// its own (its frame's pc is that OpActivate, which receives the value
// transmitted when it is next activated, or fails).

type coState uint8

const (
	coNew     coState = iota // not yet activated
	coRunning                // executing
	coResult                 // suspended after producing a result
	coWaiting                // waiting at an OpActivate
	coDone                   // exhausted
)

// Coexp is an Icon co-expression.
type Coexp struct {
	serial    int
	proc      *Proc
	entry     int     // code index of the body
	init      []Value // the creator's locals when created, for activation and refresh
	frame     *Frame  // the frame where it stopped
	activator *Coexp
	state     coState
	results   int
}

func (vm *VM) newCoexp(proc *Proc, entry int, init []Value) *Coexp {
	vm.serials["co-expression"]++
	return &Coexp{serial: vm.serials["co-expression"], proc: proc, entry: entry, init: init}
}

// transfer switches to co-expression c, delivering v (or failure, if
// !ok) to it if it is waiting at an activation, and returns the frame
// to run.
func (vm *VM) transfer(c *Coexp, v Value, ok bool) *Frame {
	vm.co = c
	var f *Frame
	switch c.state {
	case coNew:
		f = &Frame{proc: c.proc, locals: append([]Value(nil), c.init...),
			temps: make([]Value, c.proc.NTemps), pc: c.entry}
	case coResult:
		f = c.frame
	case coWaiting:
		f = c.frame
		in := &f.proc.Code[f.pc]
		if ok {
			f.temps[in.Dst.Index] = v
			f.pc = int(in.L1)
		} else {
			f.pc = int(in.L2)
		}
	default:
		panic("vm: transfer to a running or exhausted co-expression")
	}
	c.state = coRunning
	c.frame = nil
	vm.cur = f
	return f
}

// activate runs OpActivate in frame f: transmit v to co-expression c.
func (vm *VM) activate(f *Frame, in *Instr, target Value, v Value) *Frame {
	c, isCo := target.(*Coexp)
	if !isCo {
		Runerr(118, target)
	}
	if c.state == coDone {
		f.pc = int(in.L2)
		return f
	}
	cur := vm.co
	cur.frame, cur.state = f, coWaiting
	c.activator = cur
	return vm.transfer(c, v, true)
}

// coret runs OpCoRet: the running co-expression produces v and returns
// to its activator; it resumes at resume.
func (vm *VM) coret(f *Frame, v Value, resume int) *Frame {
	c := vm.co
	c.results++
	f.pc = resume
	c.frame, c.state = f, coResult
	return vm.transfer(c.activator, v, true)
}

// cofail runs OpCoFail: the running co-expression is exhausted, and its
// activator's activation fails.
func (vm *VM) cofail() *Frame {
	c := vm.co
	c.state = coDone
	return vm.transfer(c.activator, nil, false)
}

func init() {
	Ops["u^"] = &Builtin{Name: "^", Fn: func(vm *VM, a []Value) (Value, bool) {
		c, ok := a[0].(*Coexp)
		if !ok {
			Runerr(118, a[0])
		}
		if c == vm.main {
			Runerr(215, c)
		}
		return vm.newCoexp(c.proc, c.entry, c.init), true
	}}
	Ops["&current"] = &Builtin{Name: "&current", Fn: func(vm *VM, a []Value) (Value, bool) {
		return vm.co, true
	}}
	Ops["&source"] = &Builtin{Name: "&source", Fn: func(vm *VM, a []Value) (Value, bool) {
		return vm.co.activator, true
	}}
	Ops["&main"] = &Builtin{Name: "&main", Fn: func(vm *VM, a []Value) (Value, bool) {
		return vm.main, true
	}}
}
