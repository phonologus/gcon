package vm

// Invocation of values that are not procedures, following iconx's
// invoke (invoke.r) and strprc (cnv.r): a value that converts to an
// integer selects an argument; otherwise a value that converts to a
// string names a procedure, a built-in function or an operator.

// opsByName maps an operator's spelling and number of operands to its
// implementation, as iconx's operator table (odefs.h) does for string
// invocation: "-"(x) is negation and "-"(x, y) subtraction.
var opsByName = map[string][4]string{
	// spelling: implementations for 0, 1, 2 and 3 operands
	"+": {"", "u+", "+", ""}, "-": {"", "u-", "-", ""},
	"*": {"", "u*", "*", ""}, "/": {"", "u/", "/", ""},
	"%": {"", "", "%", ""}, "^": {"", "u^", "^", ""},
	"<": {"", "", "<", ""}, "<=": {"", "", "<=", ""},
	">": {"", "", ">", ""}, ">=": {"", "", ">=", ""},
	"=": {"", "u=", "=", ""}, "~=": {"", "", "~=", ""},
	"==": {"", "", "==", ""}, "~==": {"", "", "~==", ""},
	"<<": {"", "", "<<", ""}, "<<=": {"", "", "<<=", ""},
	">>": {"", "", ">>", ""}, ">>=": {"", "", ">>=", ""},
	"===": {"", "", "===", ""}, "~===": {"", "", "~===", ""},
	"||": {"", "", "||", ""}, "|||": {"", "", "|||", ""},
	"++": {"", "", "++", ""}, "--": {"", "", "--", ""},
	"**": {"", "", "**", ""},
	"~":  {"", "u~", "", ""}, "!": {"", "u!", "", ""}, "?": {"", "u?", "", ""},
	"\\": {"", `u\`, "", ""}, ".": {"", "u.", "", ""},
	"[]": {"", "", "[]", ""}, "[:]": {"", "", "", "[:]"},
	":=": {"", "", "n:=", ""}, ":=:": {"", "", "n:=:", ""},
	"<-": {"", "", "n<-", ""}, "<->": {"", "", "n<->", ""},
	"...": {"", "", "", "to"},
}

// Operators that exist only for string invocation: the compiler
// translates /x, \x, .x and the assignments inline.
func init() {
	Ops["n:="] = &Builtin{Name: ":=", NoDeref: true, Fn: func(vm *VM, a []Value) (Value, bool) {
		return a[0], setVar(a[0], Deref(a[1]))
	}}
	Ops["n:=:"] = &Builtin{Name: ":=:", NoDeref: true, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := Deref(a[0]), Deref(a[1])
		return a[0], setVar(a[0], y) && setVar(a[1], x)
	}}
	Ops["n<-"] = &Builtin{Name: "<-", NoDeref: true, Gen: func(vm *VM, a []Value) Generator {
		return &revGen{vars: append([]Value(nil), a[:1]...), olds: []Value{Deref(a[0])}, news: []Value{Deref(a[1])}}
	}}
	Ops["n<->"] = &Builtin{Name: "<->", NoDeref: true, Gen: func(vm *VM, a []Value) Generator {
		x, y := Deref(a[0]), Deref(a[1])
		return &revGen{vars: append([]Value(nil), a[:2]...), olds: []Value{x, y}, news: []Value{y, x}}
	}}
	Ops["u/"] = &Builtin{Name: "/", Fn: func(vm *VM, a []Value) (Value, bool) {
		_, isNull := a[0].AsNull()
		return a[0], isNull
	}}
	Ops[`u\`] = &Builtin{Name: `\`, Fn: func(vm *VM, a []Value) (Value, bool) {
		_, isNull := a[0].AsNull()
		return a[0], !isNull
	}}
	Ops["u."] = &Builtin{Name: ".", Fn: func(vm *VM, a []Value) (Value, bool) {
		return a[0], true
	}}
}

// callable converts v, which is not a procedure, built-in or record
// constructor, to something OpCall can invoke with nargs arguments: an
// Int, or the procedure, function or operator a string names. It raises
// error 106 if there is none.
func (vm *VM) callable(v Value, nargs int) Value {
	if i, ok := CvInt(v); ok {
		return IntVal(i)
	}
	if s, ok := CvStr(v); ok {
		if p := vm.strproc(s.s, nargs); !p.IsNone() {
			return p
		}
	}
	Runerr(106, v)
	return Value{}
}

// strproc returns the procedure, built-in function, record constructor
// or operator that name denotes with nargs arguments, or nil. As in
// iconx, a global of that name is used if there is one that the linker
// kept (Program.Invocable), and it must hold a procedure; otherwise a
// name not starting with a letter is an operator, which must take nargs
// operands.
func (vm *VM) strproc(name string, nargs int) Value {
	if name == "" {
		return Value{}
	}
	for i, g := range vm.prog.GlobalNames {
		if g == name && (vm.prog.Invocable == nil || vm.prog.Invocable[i]) {
			switch val := vm.globals[i]; val.Kind() {
			case KProc, KBuiltin, KRecordType:
				p := val.Any()
				return V(p)
			}
			return Value{}
		}
	}
	if c := name[0]; !('a' <= c && c <= 'z' || 'A' <= c && c <= 'Z') {
		impl, ok := opsByName[name]
		if !ok || nargs > 3 || impl[nargs] == "" {
			return Value{}
		}
		return BuiltinVal(Ops[impl[nargs]])
	}
	if f := Functions[name]; f != nil {
		return BuiltinVal(f)
	}
	return Value{}
}

// setVar assigns v to the variable ref, reporting whether the
// assignment succeeded; ref must be a variable (error 111).
func setVar(ref, v Value) bool {
	switch ref.Kind() {
	case KRef:
		x := ref.ref()
		*x = v
		return true
	case KListElem, KField, KSubstr, KVar:
		x, _ := asVar(ref)
		return x.Set(v)
	}
	Runerr(111, ref)
	return false
}

// revGen is a reversible assignment invoked by name: it assigns news to
// vars and produces the first variable; resumed, it restores olds and
// fails.
type revGen struct {
	vars, olds, news []Value
	done             bool
}

func (g *revGen) Next(vm *VM) (Value, bool) {
	if g.done {
		for i, v := range g.vars {
			setVar(v, g.olds[i])
		}
		return Value{}, false
	}
	g.done = true
	for i, v := range g.vars {
		if !setVar(v, g.news[i]) {
			return Value{}, false
		}
	}
	return g.vars[0], true
}

// ApplyOp marks an OpCall that is p ! L: the call's one argument is a
// list or record whose elements are the arguments (iconx's Op_Apply;
// anything else is error 126).
var ApplyOp = &Builtin{Name: "!"}

// applyArgs returns the arguments that p ! x passes.
func applyArgs(x Value) []Value {
	switch x.Kind() {
	case KList:
		l := x.List()
		return append([]Value(nil), l.Elems...)
	case KRecord:
		l := x.Record()
		return append([]Value(nil), l.Fields...)
	}
	Runerr(126, x)
	return nil
}
