package vm

import (
	"fmt"
	"strconv"
	"strings"
)

// Procedure and co-expression tracing (&trace), and the images of values
// in trace messages and error reports, as iconx's rdebug.r and
// outimage (rmisc.r).
//
// While &trace is nonzero, each call, return, suspension, failure and
// resumption of a procedure, and each co-expression activation, return
// and failure, writes a line to &errout and decrements &trace (so a
// negative value traces indefinitely). Built-in functions are not
// traced. &level counts procedure calls: it rises on a call and on a
// resumption and falls on a return, suspension or failure, and is not
// changed by co-expression switches.

const (
	stringLimit = 16 // characters of a string shown by outimage
	listLimit   = 6  // elements of a list shown by outimage
)

// OutImage returns the image of v as iconx's outimage shows it in trace
// messages and error reports. It differs from image(): long strings and
// large csets are cut short with "...", lists show their elements
// ("list_1 = [1,2,3]") and records their fields ("record p_1(1,2)").
// Inside a list or record (noimage > 0), a non-empty list is shown by
// size and a record by its number of fields.
func (vm *VM) OutImage(v Value, noimage int) string {
	switch x := v.(type) {
	case posVar:
		return "&pos = " + vm.OutImage(x.Get(), noimage)
	case subjectVar:
		return "&subject = " + vm.OutImage(x.Get(), noimage)
	case randomVar:
		return "&random = " + vm.OutImage(x.Get(), noimage)
	case traceVar:
		return "&trace = " + vm.OutImage(x.Get(), noimage)
	case errorVar:
		return "&error = " + vm.OutImage(x.Get(), noimage)
	case *Value, Var:
		return "(variable = " + vm.OutImage(Deref(v), noimage) + ")"
	}
	switch x := v.(type) {
	case *String:
		if x.Runes() <= stringLimit {
			return quote(x, '"')
		}
		i := 1
		for k := 0; k < stringLimit; k++ {
			i += x.width(i - 1)
		}
		q := quote(x.Slice(1, i), '"')
		return q[:len(q)-1] + `..."`
	case *Cset:
		if csetName(x) != "" || x.Size() <= stringLimit {
			return imageCset(x)
		}
		var b strings.Builder
		k := 0
		for m := range x.Members() {
			if k == stringLimit {
				break
			}
			b.WriteString(m)
			k++
		}
		q := quote(Text(b.String()), '\'')
		return q[:len(q)-1] + `...'`
	case *List:
		n := len(x.Elems)
		if noimage > 0 && n > 0 {
			return "list_" + strconv.Itoa(x.serial) + "(" + strconv.Itoa(n) + ")"
		}
		var b strings.Builder
		b.WriteString("list_" + strconv.Itoa(x.serial) + " = [")
		for i, e := range x.Elems {
			if i >= listLimit/2 && i < n-listLimit/2 {
				if i == listLimit/2 {
					b.WriteString("...,")
				}
				continue
			}
			b.WriteString(vm.OutImage(e, noimage+1))
			if i < n-1 {
				b.WriteByte(',')
			}
		}
		b.WriteByte(']')
		return b.String()
	case *Record:
		s := "record " + x.T.Name + "_" + strconv.Itoa(x.serial)
		switch {
		case len(x.Fields) == 0:
			return s + "()"
		case noimage > 0:
			return s + "(" + strconv.Itoa(len(x.Fields)) + ")"
		}
		parts := make([]string, len(x.Fields))
		for i, f := range x.Fields {
			parts[i] = vm.OutImage(f, noimage+1)
		}
		return s + "(" + strings.Join(parts, ",") + ")"
	}
	return vm.Image(v)
}

// tracing reports whether a trace message is due, and if so counts it
// against &trace.
func (vm *VM) tracing() bool {
	if vm.trace == 0 {
		return false
	}
	vm.trace--
	return true
}

// traceLine writes a trace message for code at line of file, indented
// by &level, as iconx's showline and showlevel do. Standard output is
// flushed first, so that the two streams stay in order.
func (vm *VM) traceLine(file string, line int, msg string) {
	var b strings.Builder
	if line > 0 {
		if len(file) > 13 {
			file = file[len(file)-13:]
		}
		fmt.Fprintf(&b, "%-13s: %4d  ", file, line)
	} else {
		b.WriteString("             :       ")
	}
	b.WriteString(strings.Repeat("| ", max(vm.level, 0)))
	b.WriteString(msg)
	b.WriteByte('\n')
	vm.out.Flush()
	vm.errout.WriteString(b.String())
	vm.errout.Flush()
}

// traceAt writes a trace message located at instruction pc of frame f.
func (vm *VM) traceAt(f *Frame, pc int, msg string) {
	line := 0
	if f != nil && pc >= 0 && pc < len(f.proc.Code) {
		line = int(f.proc.Code[pc].Line)
	}
	file := ""
	if f != nil {
		file = f.proc.File
	}
	vm.traceLine(file, line, msg)
}

// traceCall traces a call of procedure p from instruction pc of frame f.
func (vm *VM) traceCall(f *Frame, pc int, p *Proc, args []Value) {
	parts := make([]string, len(args))
	for i, a := range args {
		parts[i] = vm.OutImage(a, 0)
	}
	vm.traceAt(f, pc, p.Name+"("+strings.Join(parts, ",")+")")
}

// traceCo traces a co-expression switch from the running co-expression
// to next, made by the instruction at pc of frame f: kind is "activate"
// (transmitting v), "return" (producing v) or "fail".
func (vm *VM) traceCo(f *Frame, pc int, kind string, v Value, next *Coexp) {
	msg := f.proc.Name + "; co-expression_" + strconv.Itoa(vm.co.serial) + " "
	switch kind {
	case "activate":
		msg += ": " + vm.OutImage(v, 0) + " @ "
	case "return":
		msg += "returned " + vm.OutImage(v, 0) + " to "
	case "fail":
		msg += "failed to "
	}
	vm.traceAt(f, pc, msg+"co-expression_"+strconv.Itoa(next.serial))
}

// traceVar is the variable &trace.
type traceVar struct{ vm *VM }

func (k traceVar) Get() Value { return k.vm.trace }
func (k traceVar) Set(v Value) bool {
	k.vm.trace = MustInt(v)
	return true
}

func init() {
	Ops["&trace"] = &Builtin{Name: "&trace", Fn: func(vm *VM, a []Value) (Value, bool) {
		return traceVar{vm}, true
	}}
	Ops["&level"] = &Builtin{Name: "&level", Fn: func(vm *VM, a []Value) (Value, bool) {
		return Int(vm.level), true
	}}
}

// asgnOps are markers naming the operator an OpAssign belongs to, for
// the traceback, where it is not plain := (the compiler marks the first
// assignment of :=:, <- and <->).
var asgnOps = map[string]*Builtin{}

// AsgnOp returns the marker for assignment operator name.
func AsgnOp(name string) *Builtin {
	b := asgnOps[name]
	if b == nil {
		b = &Builtin{Name: name}
		asgnOps[name] = b
	}
	return b
}

// traceback writes iconx's traceback after a run-time error message
// (rdebug.r, tracebk): each active procedure call from the outermost,
// with its parameters' current values and the line it was called from,
// then the operation that failed.
func (vm *VM) traceback(w interface{ WriteString(string) (int, error) }) {
	f := vm.cur
	if f == nil || f.pc >= len(f.proc.Code) {
		return
	}
	w.WriteString("Traceback:\n")
	var chain []*Frame
	for g := f; g != nil; g = g.caller {
		chain = append(chain, g)
	}
	for i := len(chain) - 1; i >= 0; i-- {
		g := chain[i]
		parts := make([]string, g.proc.NParams)
		for j := range parts {
			parts[j] = vm.OutImage(g.locals[j], 0)
		}
		s := g.proc.Name + "(" + strings.Join(parts, ",") + ")"
		s += vm.calledFrom(g)
		w.WriteString(s + "\n")
	}
	in := &f.proc.Code[f.pc]
	w.WriteString(vm.offending(f, f.pc) + fmt.Sprintf(" from line %d in %s\n", in.Line, f.proc.File))
}

// offending describes the operation at instruction pc of frame f as
// iconx's ttrace does: "{x + y}", "{x[i]}", "f(args)" and so on, showing
// the operands of operators that take variables (subscripts, sections,
// assignments, ! and ?) as variables.
func (vm *VM) offending(f *Frame, pc int) string {
	in := &f.proc.Code[pc]
	val := func(o Operand) string { return vm.OutImage(Deref(vm.get(f, o)), 0) }
	// converted shows the left operand of binary operator op as iconx
	// does: already converted to the type op wants, since iconx converts
	// its operands in place, the left first.
	converted := func(op string, x Value) string {
		var c Value
		ok := false
		switch op {
		case "+", "-", "*", "/", "%", "^", "<", "<=", ">", ">=", "=", "~=":
			c, ok = CvNum(x)
		case "||", "==", "~==", "<<", "<<=", ">>", ">>=":
			c, ok = CvStr(x)
		case "++", "--", "**":
			if _, isSet := x.(*Set); !isSet {
				c, ok = CvCset(x)
			}
		}
		if !ok {
			c = x
		}
		return vm.OutImage(c, 0)
	}
	vari := func(o Operand) string { return vm.OutImage(vm.ref(f, o), 0) }
	args := func(ops []Operand) string {
		parts := make([]string, len(ops))
		for i, o := range ops {
			parts[i] = val(o)
		}
		return strings.Join(parts, ",")
	}
	// operator describes operator b applied to ops.
	operator := func(b *Builtin, ops []Operand) string {
		switch {
		case b.Name == "[]" && len(ops) == 2:
			return "{" + vari(ops[0]) + "[" + val(ops[1]) + "]}"
		case b.Name == "[:]" && len(ops) == 3:
			return "{" + vari(ops[0]) + "[" + val(ops[1]) + ":" + val(ops[2]) + "]}"
		case b.Name == "limit":
			return "limit counter: " + val(ops[0])
		case b.Name == "[...]":
			return "[ ... ]"
		case strings.HasPrefix(b.Name, "&"):
			return "bad keyword reference"
		case strings.HasPrefix(b.Name, ".") && len(ops) == 1:
			// iconx shows the word "field", not the field's name.
			return "{" + val(ops[0]) + " . field}"
		case b.Name == "to":
			by := "1"
			if len(ops) > 2 {
				by = val(ops[2])
			}
			return "{" + val(ops[0]) + " to " + val(ops[1]) + " by " + by + "}"
		case (b.Name == "!" || b.Name == "?") && len(ops) == 1:
			return "{" + b.Name + vari(ops[0]) + "}"
		case len(ops) == 1:
			return "{" + b.Name + val(ops[0]) + "}"
		case len(ops) == 2:
			return "{" + converted(b.Name, Deref(vm.get(f, ops[0]))) + " " + b.Name + " " + val(ops[1]) + "}"
		}
		return b.Name + "(" + args(ops) + ")"
	}
	switch in.Op {
	case OpOper:
		return operator(in.Fn, in.Args)
	case OpAssign:
		name := ":="
		if in.Fn != nil {
			name = in.Fn.Name
		}
		x := val(in.X)
		if name == ":=:" || name == "<->" {
			x = vari(in.X)
		}
		return "{" + vari(in.Dst) + " " + name + " " + x + "}"
	case OpActivate:
		v := "&null"
		if len(in.Args) > 0 {
			v = val(in.Args[0])
		}
		return "{" + v + " @ " + val(in.X) + "}"
	case OpScanEnter:
		return "{" + val(in.X) + " ? ..}"
	case OpResume:
		// A resumed built-in generator: its call is the instruction
		// before (see compile's call).
		if pc > 0 && f.proc.Code[pc-1].Op == OpCall && f.proc.Code[pc-1].H == in.H {
			return vm.offending(f, pc-1)
		}
	case OpCall:
		switch fv := Deref(vm.get(f, in.X)).(type) {
		case *Builtin:
			if Functions[fv.Name] == fv || fv.NoDeref {
				return fv.Name + "(" + args(in.Args) + ")"
			}
			return operator(fv, in.Args)
		case *Proc:
			return fv.Name + "(" + args(in.Args) + ")"
		default:
			// Invoked by name: shown by the name it resolved to, if any.
			if s, ok := CvStr(fv); ok {
				switch p := vm.strproc(s.s, len(in.Args)).(type) {
				case *Builtin:
					return p.Name + "(" + args(in.Args) + ")"
				case *Proc:
					return p.Name + "(" + args(in.Args) + ")"
				case *RecordType:
					return p.Name + "(" + args(in.Args) + ")"
				}
			}
			return vm.OutImage(fv, 0) + "(" + args(in.Args) + ")"
		}
	}
	return "{...}"
}

// calledFrom returns " from line n in file" for the call that made the
// active frame f. A frame without a caller is main's (called from
// nowhere) or the base of the running co-expression, a copy of its
// creator's frame, which was called from where the creator was.
func (vm *VM) calledFrom(f *Frame) string {
	c := f.caller
	if c == nil {
		return vm.co.from
	}
	return fmt.Sprintf(" from line %d in %s", c.proc.Code[c.pc].Line, c.proc.File)
}
