package vm

import "unicode/utf8"

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

func init() {
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

// Unicode functions (docs/design.md, "Strings and Unicode").
func init() {
	// runes(x) and bytes(x) relabel a string's bytes. A cset has no unit:
	// runes(c) is c, and bytes(c) is the cset of the bytes that match c in
	// a bytes subject.
	fn("runes", func(vm *VM, a []Value) (Value, bool) {
		if c, ok := arg(a, 0).(*Cset); ok {
			return c, true
		}
		s := MustStr(arg(a, 0))
		return &String{s: s.s, unit: TextUnit, ascii: s.ascii}, true
	})
	fn("bytes", func(vm *VM, a []Value) (Value, bool) {
		if c, ok := arg(a, 0).(*Cset); ok {
			return c.AsBytes(), true
		}
		s := MustStr(arg(a, 0))
		return &String{s: s.s, unit: BytesUnit, ascii: s.ascii}, true
	})
	// len(s) is the number of characters in s; *s is its length in bytes.
	fn("len", func(vm *VM, a []Value) (Value, bool) {
		return Int(MustStr(arg(a, 0)).Runes()), true
	})
	fn("utf8valid", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		return s, utf8.ValidString(s.s)
	})
	fn("uchar", func(vm *VM, a []Value) (Value, bool) {
		i := MustInt(arg(a, 0))
		r := utf8.RuneError
		if i >= 0 && i <= utf8.MaxRune {
			r = rune(i) // string(r) gives U+FFFD for surrogates, as Go does
		}
		return Text(string(r)), true
	})
	fn("char", func(vm *VM, a []Value) (Value, bool) {
		i := MustInt(arg(a, 0))
		if i < 0 || i > 255 {
			Runerr(205, i)
		}
		if i < utf8.RuneSelf {
			return Text(string(rune(i))), true
		}
		return NewString(string([]byte{byte(i)}), BytesUnit), true
	})
	fn("ord", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		if s.Runes() != 1 {
			Runerr(205, s)
		}
		if s.unit == BytesUnit || s.IsASCII() {
			return Int(s.s[0]), true
		}
		r, _ := utf8.DecodeRuneInString(s.s) // U+FFFD for a lone byte
		return Int(r), true
	})
}
