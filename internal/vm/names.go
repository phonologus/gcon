package vm

import (
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// proc, args, name, variable, function and the small system functions
// collect, chdir and delay, as iconx's (fconv.r, fmisc.r, fsys.r,
// rdebug.r's get_name, rmisc.r's getvar).

// funcArity is args(f) for the built-in functions, as iconx reports it
// (its procedure blocks' nparam): the number of parameters, or for a
// function taking a variable number, -(n+1) where n are fixed.
var funcArity = map[string]int{
	"abs": 1, "acos": 1, "any": 4, "args": 1, "asin": 1, "atan": 2,
	"bal": 6, "center": 3, "char": 1, "chdir": 1, "close": 1, "collect": 2,
	"copy": 1, "cos": 1, "cset": 1, "delay": 1, "delete": 2, "detab": -2,
	"display": 2, "dtor": 1, "entab": -2, "errorclear": 0, "exit": 1, "exp": 1,
	"find": 4, "flush": 1, "function": 0, "get": 1, "getch": 0, "getche": 0,
	"getenv": 1, "iand": 2, "icom": 1, "image": 1, "insert": 3, "integer": 1,
	"ior": 2, "ishift": 2, "ixor": 2, "kbhit": 0, "key": 1, "left": 3,
	"list": 2, "loadfunc": 2, "log": 2, "many": 4, "map": 3, "match": 4,
	"member": 2, "move": 1, "name": 1, "numeric": 1, "open": 2, "ord": 1,
	"pop": 1, "pos": 1, "proc": 2, "pull": 1, "push": -2, "put": -2,
	"read": 1, "reads": 2, "real": 1, "remove": 1, "rename": 2, "repl": 2,
	"reverse": 1, "right": 3, "rtod": 1, "runerr": -2, "seek": 2, "seq": 2,
	"serial": 1, "set": 1, "sin": 1, "sort": 2, "sortf": 2, "sqrt": 1,
	"stop": -1, "string": 1, "system": 1, "tab": 1, "table": 1, "tan": 1,
	"trim": 2, "type": 1, "upto": 4, "variable": 1, "where": 1, "write": -1,
	"writes": -1,
	// gcon's own functions
	"bytes": 1, "len": 1, "runes": 1, "uchar": 1, "utf8valid": 1,
}

// opArity returns the number of operands of an operator implementation
// that string invocation can reach, or -1.
func opArity(b *Builtin) int {
	opArityOnce.Do(func() {
		opArities = map[*Builtin]int{}
		for _, impl := range opsByName {
			for n, k := range impl {
				if k != "" {
					opArities[Ops[k]] = n
				}
			}
		}
	})
	if n, ok := opArities[b]; ok {
		return n
	}
	return -1
}

var (
	opArityOnce sync.Once
	opArities   map[*Builtin]int
)

// isFunction reports whether b is a built-in function (not an
// operator).
func isFunction(b *Builtin) bool { return Functions[b.Name] == b }

func init() {
	// proc(x, i): x if it is a procedure; else the procedure, function
	// or operator a string names, an operator taking i operands (1 by
	// default); i = 0 finds only built-in functions.
	fn("proc", func(vm *VM, a []Value) (Value, bool) {
		x := arg(a, 0)
		switch x.(type) {
		case *Proc, *Builtin, *RecordType:
			return x, true
		}
		s, ok := CvStr(x)
		if !ok {
			return nil, false
		}
		i := Int(1)
		if _, null := arg(a, 1).(Null); !null {
			i = MustInt(a[1])
		}
		if i < 0 || i > 3 {
			Runerr(205, i)
		}
		if i == 0 {
			if f := Functions[s.s]; f != nil && s.s != "" && isLetter(s.s[0]) {
				return f, true
			}
			return nil, false
		}
		p := vm.strproc(s.s, int(i))
		return p, p != nil
	})
	// args(p): the number of parameters of procedure p, negative if it
	// takes a variable number (-n for n parameters, the last a list).
	fn("args", func(vm *VM, a []Value) (Value, bool) {
		switch p := arg(a, 0).(type) {
		case *Proc:
			if p.Variadic {
				return Int(-p.NParams), true
			}
			return Int(p.NParams), true
		case *RecordType:
			return Int(len(p.Fields)), true
		case *Builtin:
			if isFunction(p) {
				return Int(funcArity[p.Name]), true
			}
			if p.NoDeref {
				return Int(2), true
			}
			return Int(opArity(p)), true
		}
		Runerr(106, arg(a, 0))
		return nil, false
	})
	// function(): the names of the built-in functions, in order.
	gen("function", func(vm *VM, a []Value) Generator {
		names := make([]string, 0, len(Functions))
		for n := range Functions {
			names = append(names, n)
		}
		sort.Strings(names)
		return &stringsGen{names}
	})
	// name(v): the name of variable v.
	Functions["name"] = &Builtin{Name: "name", RefFirst: true, Fn: func(vm *VM, a []Value) (Value, bool) {
		v := arg(a, 0)
		switch v.(type) {
		case *Value, Var:
		default:
			Runerr(111, v)
		}
		return Text(vm.varName(v)), true
	}}
	// variable(s): the variable named s: a keyword, a parameter, local
	// or static of the running procedure, or a global.
	fn("variable", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0)).s
		return vm.lookupVar(s)
	})
	// collect(region, bytes): iconx's garbage collection. gcon's memory
	// is Go's; the collection is counted, for &collections, as iconx
	// counts it: in the total, and for regions 1, 2 and 3 (static,
	// string, block) in that region's count.
	fn("collect", func(vm *VM, a []Value) (Value, bool) {
		var region Int
		if _, null := arg(a, 0).(Null); !null {
			region = MustInt(a[0])
		}
		if _, null := arg(a, 1).(Null); !null && MustInt(a[1]) < 0 {
			Runerr(205, a[1])
		}
		if region < 0 || region > 3 {
			Runerr(205, region)
		}
		vm.collections[0]++
		if region > 0 {
			vm.collections[region]++
		}
		return NullValue, true
	})
	fn("chdir", func(vm *VM, a []Value) (Value, bool) {
		if err := os.Chdir(MustStr(arg(a, 0)).s); err != nil {
			return nil, false
		}
		return NullValue, true
	})
	// delay(i): pause for i milliseconds, writing out pending output
	// first, as iconx's stdout would have been.
	fn("delay", func(vm *VM, a []Value) (Value, bool) {
		n := MustInt(arg(a, 0))
		vm.out.Flush()
		if n > 0 {
			time.Sleep(time.Duration(n) * time.Millisecond)
		}
		return NullValue, true
	})
}

func isLetter(c byte) bool { return 'a' <= c && c <= 'z' || 'A' <= c && c <= 'Z' }

// varName returns the name of variable v, as iconx's get_name: an
// identifier, a keyword, L[i] for a list element (iconx writes "L"
// literally), rec.field, T[key image], or a substring's variable
// followed by [i:j].
func (vm *VM) varName(v Value) string {
	switch x := v.(type) {
	case *SubstrVar:
		return vm.varName(x.v) + "[" + strconv.Itoa(x.pos) + ":" + strconv.Itoa(x.pos+x.n) + "]"
	case ListElem:
		return "L[" + strconv.Itoa(x.I+1) + "]"
	case FieldVar:
		return x.R.T.Name + "." + x.R.T.Fields[x.I]
	case TableElem:
		return "T[" + vm.Image(x.K) + "]"
	case posVar:
		return "&pos"
	case subjectVar:
		return "&subject"
	case randomVar:
		return "&random"
	case traceVar:
		return "&trace"
	case errorVar:
		return "&error"
	case *Value:
		for i := range vm.globals {
			if x == &vm.globals[i] {
				n := vm.prog.GlobalNames[i]
				if j := strings.IndexByte(n, ' '); j >= 0 {
					return n[j+1:] // a static, "proc name"
				}
				return n
			}
		}
		if f := vm.cur; f != nil {
			for i := range f.locals {
				if x == &f.locals[i] && i < len(f.proc.LocalNames) {
					return f.proc.LocalNames[i]
				}
			}
		}
	}
	return "?"
}

// lookupVar finds the variable named s for variable(s).
func (vm *VM) lookupVar(s string) (Value, bool) {
	switch s {
	case "&error":
		return errorVar{vm}, true
	case "&pos":
		return posVar{vm}, true
	case "&random":
		return randomVar{vm}, true
	case "&subject":
		return subjectVar{vm}, true
	case "&trace":
		return traceVar{vm}, true
	}
	if strings.HasPrefix(s, "&") {
		return nil, false
	}
	if f := vm.cur; f != nil {
		for i, n := range f.proc.LocalNames {
			if n == s {
				return &f.locals[i], true
			}
		}
		for i, n := range vm.prog.GlobalNames {
			if n == f.proc.Name+" "+s {
				return &vm.globals[i], true
			}
		}
	}
	for i, n := range vm.prog.GlobalNames {
		if n == s && (vm.prog.Invocable == nil || vm.prog.Invocable[i]) {
			return &vm.globals[i], true
		}
	}
	return nil, false
}

// adjustArgs fits the arguments of a call of built-in b to its
// parameters, as iconx's invoke does: missing ones are &null and extra
// ones are dropped; a function taking a variable number gets at least
// its fixed parameters.
func adjustArgs(b *Builtin, args []Value) []Value {
	n := -1
	switch {
	case isFunction(b):
		ar, ok := funcArity[b.Name]
		if !ok {
			return args
		}
		if ar < 0 {
			return withNulls(args, -ar-1)
		}
		n = ar
	case b.NoDeref:
		n = 2
	default:
		n = opArity(b)
	}
	if n < 0 {
		return args
	}
	return withNulls(args, n)[:n]
}

// withNulls returns args with at least n elements, the added ones
// &null, in one allocation.
func withNulls(args []Value, n int) []Value {
	if len(args) >= n {
		return args
	}
	if cap(args) < n {
		a := make([]Value, len(args), n)
		copy(a, args)
		args = a
	}
	for len(args) < n {
		args = append(args, NullValue)
	}
	return args
}

func init() {
	// display(i, f): the current co-expression, then the identifiers of
	// the i innermost procedure activations (default &level) with their
	// values, then the globals, written to f (default &errout), as
	// iconx's display and xdisp.
	fn("display", func(vm *VM, a []Value) (Value, bool) {
		n := Int(vm.level)
		if _, null := arg(a, 0).(Null); !null {
			n = MustInt(a[0])
		}
		f := vm.errfile
		switch x := arg(a, 1).(type) {
		case Null:
		case *File:
			f = x
		default:
			Runerr(105, x)
		}
		if f.w == nil || f.closed {
			Runerr(213, f)
		}
		if n < 0 {
			Runerr(205, n)
		}
		if n > Int(vm.level) {
			n = Int(vm.level)
		}
		s := "co-expression_" + strconv.Itoa(vm.co.serial) + "(" + strconv.Itoa(vm.co.results) + ")\n\n" + vm.listing(int(n))
		vm.emit(f, []byte(s), true)
		return NullValue, true
	})
}

// listing returns the identifiers of the n innermost procedure
// activations and the globals with their values, as iconx's xdisp (for
// display and the termination dump).
func (vm *VM) listing(n int) string {
	var b strings.Builder
	for fr := vm.cur; fr != nil && n > 0; fr, n = fr.caller, n-1 {
		p := fr.proc
		b.WriteString(p.Name + " local identifiers:\n")
		for i, name := range p.LocalNames {
			b.WriteString("   " + name + " = " + vm.OutImage(fr.locals[i], 0) + "\n")
		}
		for _, g := range p.Statics {
			name := vm.prog.GlobalNames[g]
			name = name[strings.IndexByte(name, ' ')+1:]
			b.WriteString("   " + name + " = " + vm.OutImage(vm.globals[g], 0) + "\n")
		}
	}
	b.WriteString("\nglobal identifiers:\n")
	var gs []int
	for i, name := range vm.prog.GlobalNames {
		if strings.IndexByte(name, ' ') < 0 && (vm.prog.Invocable == nil || vm.prog.Invocable[i]) {
			gs = append(gs, i)
		}
	}
	sort.Slice(gs, func(i, j int) bool { return vm.prog.GlobalNames[gs[i]] < vm.prog.GlobalNames[gs[j]] })
	for _, i := range gs {
		b.WriteString("   " + vm.prog.GlobalNames[i] + " = " + vm.OutImage(vm.globals[i], 0) + "\n")
	}
	return b.String()
}
