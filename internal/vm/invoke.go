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
	"~":  {"", "u~", "", ""}, "!": {"", "u!", "", ""},
	"\\": {"", `u\`, "", ""}, ".": {"", "u.", "", ""},
	"[]": {"", "", "[]", ""}, "[:]": {"", "", "", "[:]"},
	"...": {"", "", "", "to"},
}

// Operators that exist only for string invocation: the compiler
// translates /x, \x and .x inline.
func init() {
	Ops["u/"] = &Builtin{Name: "/", Fn: func(vm *VM, a []Value) (Value, bool) {
		_, isNull := a[0].(Null)
		return a[0], isNull
	}}
	Ops[`u\`] = &Builtin{Name: `\`, Fn: func(vm *VM, a []Value) (Value, bool) {
		_, isNull := a[0].(Null)
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
		return i
	}
	if s, ok := CvStr(v); ok {
		if p := vm.strproc(s.s, nargs); p != nil {
			return p
		}
	}
	Runerr(106, v)
	return nil
}

// strproc returns the procedure, built-in function, record constructor
// or operator that name denotes with nargs arguments, or nil. As in
// iconx, a global of that name is used if there is one that the linker
// kept (Program.Invocable), and it must hold a procedure; otherwise a
// name not starting with a letter is an operator, which must take nargs
// operands.
func (vm *VM) strproc(name string, nargs int) Value {
	if name == "" {
		return nil
	}
	for i, g := range vm.prog.GlobalNames {
		if g == name && (vm.prog.Invocable == nil || vm.prog.Invocable[i]) {
			switch p := vm.globals[i].(type) {
			case *Proc, *Builtin, *RecordType:
				return p
			}
			return nil
		}
	}
	if c := name[0]; !('a' <= c && c <= 'z' || 'A' <= c && c <= 'Z') {
		impl, ok := opsByName[name]
		if !ok || nargs > 3 || impl[nargs] == "" {
			return nil
		}
		return Ops[impl[nargs]]
	}
	if f := Functions[name]; f != nil {
		return f
	}
	return nil
}
