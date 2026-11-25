package vm

import "runtime"

// Error conversion (&error), following iconx's err_msg (errmsg.r).
//
// While &error is nonzero, a run-time error does not stop the program:
// &error is decremented, &errornumber, &errortext and &errorvalue record
// the error, and the instruction that raised it fails. Only instructions
// with a failure label can raise errors that are converted; an error
// anywhere else is reported as usual.

// convert converts run-time error e to failure if &error allows it,
// reporting whether it did. The running frame then continues at the
// failing instruction's failure label.
func (vm *VM) convert(e *RuntimeError) bool {
	vm.errNum, vm.errVal, vm.haveErrVal = e.Num, e.Value, e.HasValue
	if !e.HasValue {
		vm.errVal = NullValue
	}
	f := vm.cur
	if vm.errorKw == 0 || f == nil || f.pc >= len(f.proc.Code) {
		return false
	}
	if e.Fatal {
		return false // as in iconx, stack overflow is never converted
	}
	in := &f.proc.Code[f.pc]
	switch in.Op {
	case OpOper, OpAssign, OpActivate, OpScanEnter:
	case OpCall, OpResume:
		f.temps[in.H] = Value{}
	default:
		return false
	}
	vm.errorKw--
	// The output of an interrupted write call stays written.
	if vm.partialFile != nil {
		vm.emit(vm.partialFile, vm.partial, true)
		vm.partial, vm.partialFile = nil, nil
	}
	f.pc = int(in.L2)
	return true
}

// errorVar is the variable &error.
type errorVar struct{ vm *VM }

func (k errorVar) Get() Value { return V(k.vm.errorKw) }
func (k errorVar) Set(v Value) bool {
	k.vm.errorKw = MustInt(v)
	return true
}

func init() {
	Ops["&error"] = &Builtin{Name: "&error", Fn: func(vm *VM, a []Value) (Value, bool) {
		return V(errorVar{vm}), true
	}}
	Ops["&errornumber"] = &Builtin{Name: "&errornumber", Fn: func(vm *VM, a []Value) (Value, bool) {
		if vm.errNum == 0 {
			return Value{}, false
		}
		return V(Int(vm.errNum)), true
	}}
	Ops["&errortext"] = &Builtin{Name: "&errortext", Fn: func(vm *VM, a []Value) (Value, bool) {
		if vm.errNum == 0 {
			return Value{}, false
		}
		return V(Text(errorText[vm.errNum])), true
	}}
	Ops["&errorvalue"] = &Builtin{Name: "&errorvalue", Fn: func(vm *VM, a []Value) (Value, bool) {
		if !vm.haveErrVal {
			return Value{}, false
		}
		return vm.errVal, true
	}}
	fn("errorclear", func(vm *VM, a []Value) (Value, bool) {
		vm.errNum, vm.errVal, vm.haveErrVal = 0, NullValue, false
		return NullValue, true
	})
	fn("runerr", func(vm *VM, a []Value) (Value, bool) {
		i := MustInt(arg(a, 0))
		if i <= 0 {
			Runerr(205, V(i))
		}
		if len(a) > 1 {
			Runerr(int(i), a[1])
		}
		Runerr(int(i))
		return Value{}, false
	})
}

// &line and &file: where the running code is.
func init() {
	Ops["&line"] = &Builtin{Name: "&line", Fn: func(vm *VM, a []Value) (Value, bool) {
		f := vm.cur
		return V(Int(f.proc.Code[f.pc].Line)), true
	}}
	Ops["&file"] = &Builtin{Name: "&file", Fn: func(vm *VM, a []Value) (Value, bool) {
		return V(Text(vm.cur.proc.FileAt(vm.cur.pc))), true
	}}
}

// Features lists what &features generates: the strings iconx uses for
// the features gcon has, in iconx's order (src/h/features.h).
var Features = func() []string {
	f := []string{"UNIX"}
	switch runtime.GOOS {
	case "windows":
		f[0] = "MS Windows"
	case "darwin":
		f = append(f, "Macintosh") // as iconx's macOS build reports
	}
	return append(f, "ASCII", "co-expressions", "environment variables",
		"keyboard functions", "large integers", "pipes", "system function")
}()

type stringsGen struct{ s []string }

func (g *stringsGen) Next(vm *VM) (Value, bool) {
	if len(g.s) == 0 {
		return Value{}, false
	}
	v := Text(g.s[0])
	g.s = g.s[1:]
	return V(v), true
}

func init() {
	Ops["&features"] = &Builtin{Name: "&features", Gen: func(vm *VM, a []Value) Generator {
		return &stringsGen{Features}
	}}
}
