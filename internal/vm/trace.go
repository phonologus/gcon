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
