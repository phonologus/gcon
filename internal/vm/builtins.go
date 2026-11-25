package vm

// Functions holds the built-in functions gcon implements, by name. A
// program that uses another of Icon's built-in functions gets a stub
// that raises error 121 ("function not supported") when called.
var Functions = map[string]*Builtin{}

func fn(name string, f func(vm *VM, a []Value) (Value, bool)) {
	Functions[name] = &Builtin{Name: name, Fn: f}
}

// arg returns a[i], or &null if absent.
func arg(a []Value, i int) Value {
	if i < len(a) {
		return a[i]
	}
	return NullValue
}

// Unsupported returns a stub for a built-in function gcon lacks.
func Unsupported(name string) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		Runerr(121)
		return nil, false
	}}
}

// writeArgs writes the arguments of write or writes.
func (vm *VM) writeArgs(a []Value) Value {
	var last Value = Text("")
	for _, v := range a {
		switch x := v.(type) {
		case Null:
			last = Text("")
			continue
		case *String:
			vm.out.WriteString(x.s)
		default:
			s, ok := CvStr(x)
			if !ok {
				Runerr(109, x)
			}
			vm.out.WriteString(s.s)
		}
		last = v
	}
	return last
}

func init() {
	fn("write", func(vm *VM, a []Value) (Value, bool) {
		v := vm.writeArgs(a)
		vm.out.WriteByte('\n')
		return v, true
	})
	fn("writes", func(vm *VM, a []Value) (Value, bool) {
		return vm.writeArgs(a), true
	})
	fn("stop", func(vm *VM, a []Value) (Value, bool) {
		vm.out.Flush()
		for _, v := range a {
			if _, ok := v.(Null); ok {
				continue
			}
			s, ok := CvStr(v)
			if !ok {
				Runerr(109, v)
			}
			vm.errout.WriteString(s.s)
		}
		vm.errout.WriteByte('\n')
		vm.Exit(1)
		return nil, false
	})
	fn("exit", func(vm *VM, a []Value) (Value, bool) {
		code := 0
		if _, ok := arg(a, 0).(Null); !ok {
			code = int(MustInt(a[0]))
		}
		vm.Exit(code)
		return nil, false
	})
	fn("image", func(vm *VM, a []Value) (Value, bool) {
		return Text(vm.Image(arg(a, 0))), true
	})
	fn("type", func(vm *VM, a []Value) (Value, bool) {
		return Text(TypeName(arg(a, 0))), true
	})
	fn("string", func(vm *VM, a []Value) (Value, bool) {
		s, ok := CvStr(arg(a, 0))
		return s, ok
	})
	fn("integer", func(vm *VM, a []Value) (Value, bool) {
		i, ok := CvInt(arg(a, 0))
		return i, ok
	})
	fn("list", func(vm *VM, a []Value) (Value, bool) {
		n := 0
		if _, ok := arg(a, 0).(Null); !ok {
			n = int(MustInt(a[0]))
			if n < 0 {
				Runerr(205, a[0])
			}
		}
		e := make([]Value, n)
		for i := range e {
			e[i] = arg(a, 1)
		}
		return vm.NewList(e), true
	})
	fn("put", func(vm *VM, a []Value) (Value, bool) {
		l := mustList(arg(a, 0))
		if len(a) < 2 {
			l.Elems = append(l.Elems, NullValue)
		} else {
			l.Elems = append(l.Elems, a[1:]...)
		}
		return l, true
	})
	fn("push", func(vm *VM, a []Value) (Value, bool) {
		l := mustList(arg(a, 0))
		if len(a) < 2 {
			a = append(a, NullValue)
		}
		for _, v := range a[1:] {
			l.Elems = append([]Value{v}, l.Elems...)
		}
		return l, true
	})
	get := func(vm *VM, a []Value) (Value, bool) {
		l := mustList(arg(a, 0))
		if len(l.Elems) == 0 {
			return nil, false
		}
		v := l.Elems[0]
		l.Elems = l.Elems[1:]
		return v, true
	}
	fn("get", get)
	fn("pop", get)
	fn("pull", func(vm *VM, a []Value) (Value, bool) {
		l := mustList(arg(a, 0))
		if len(l.Elems) == 0 {
			return nil, false
		}
		v := l.Elems[len(l.Elems)-1]
		l.Elems = l.Elems[:len(l.Elems)-1]
		return v, true
	})
}

func mustList(v Value) *List {
	l, ok := v.(*List)
	if !ok {
		Runerr(108, v)
	}
	return l
}
