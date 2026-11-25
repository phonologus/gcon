package vm

import (
	"strconv"
	"unicode/utf8"
)

// Value is an Icon value: one of the types below. A Value may also be a
// variable reference (see Var) where an expression produces a variable;
// operations dereference their operands with Deref.
type Value any

// Null is the type of &null.
type Null struct{}

// NullValue is &null.
var NullValue Value = Null{}

// Int is an Icon integer. Large integers are not yet supported:
// arithmetic that overflows is run-time error 203.
type Int int64

// Unit is the character unit of a string: text (UTF-8, read as runes in
// the manner of Go) or bytes. See docs/design.md, "Strings and Unicode".
type Unit uint8

const (
	TextUnit Unit = iota
	BytesUnit
)

// String is an Icon string: its bytes, its unit, and cached facts about
// them. Strings are immutable.
type String struct {
	s     string
	unit  Unit
	ascii int8 // 0 unknown, 1 all ASCII, -1 not
	n     int  // character count, -1 unknown
	// A cursor: character position cpos (0 if none) is at byte offset
	// coff. Scanning moves through a subject in small steps, so finding
	// an offset from the last one found is cheap.
	cpos, coff int
}

// NewString returns a string with the given bytes and unit.
func NewString(s string, u Unit) *String { return &String{s: s, unit: u, n: -1} }

// Text returns a text string.
func Text(s string) *String { return NewString(s, TextUnit) }

// Go returns the string's bytes.
func (s *String) Go() string { return s.s }

// IsASCII reports whether every byte of s is ASCII.
func (s *String) IsASCII() bool {
	if s.ascii == 0 {
		s.ascii = 1
		for i := 0; i < len(s.s); i++ {
			if s.s[i] >= utf8.RuneSelf {
				s.ascii = -1
				break
			}
		}
	}
	return s.ascii > 0
}

// IsBytes reports whether s counts in bytes. ASCII strings behave the
// same in either unit.
func (s *String) IsBytes() bool { return s.unit == BytesUnit }

// Unit returns the string's unit.
func (s *String) Unit() Unit { return s.unit }

// Len returns the number of characters in s. In text, each invalid byte
// is one character, as utf8.RuneCountInString counts.
func (s *String) Len() int {
	if s.n < 0 {
		if s.unit == BytesUnit || s.IsASCII() {
			s.n = len(s.s)
		} else {
			s.n = utf8.RuneCountInString(s.s)
		}
	}
	return s.n
}

// offset returns the byte offset of character position i (1-based,
// 1 <= i <= Len()+1). For non-ASCII text it steps from the cursor, or
// from the start if the cursor is beyond i, and leaves the cursor at i.
//
// TODO: the sparse offset index of docs/design.md ("Fast positions"),
// for random access far from the cursor.
func (s *String) offset(i int) int {
	if s.unit == BytesUnit || s.IsASCII() {
		return i - 1
	}
	k, off := 1, 0
	if s.cpos > 0 && s.cpos <= i {
		k, off = s.cpos, s.coff
	}
	for ; k < i; k++ {
		_, w := utf8.DecodeRuneInString(s.s[off:])
		off += w
	}
	s.cpos, s.coff = i, off
	return off
}

// Slice returns the substring from character position i up to j
// (1 <= i <= j <= Len()+1).
func (s *String) Slice(i, j int) *String {
	a := s.offset(i)
	b := s.offset(j)
	r := &String{s: s.s[a:b], unit: s.unit, n: j - i}
	if s.ascii > 0 {
		r.ascii = 1
	}
	return r
}

// Concat joins a and b. The result is text if either operand is
// non-ASCII text, else bytes if either is non-ASCII bytes.
func Concat(a, b *String) *String {
	u := TextUnit
	switch {
	case a.unit == TextUnit && !a.IsASCII(), b.unit == TextUnit && !b.IsASCII():
	case !a.IsASCII() || !b.IsASCII():
		u = BytesUnit
	}
	r := &String{s: a.s + b.s, unit: u, n: -1}
	if a.ascii > 0 && b.ascii > 0 {
		r.ascii = 1
	}
	return r
}

// List is an Icon list.
type List struct {
	Elems  []Value
	serial int
}

// Proc is a compiled Icon procedure.
type Proc struct {
	Name     string
	File     string
	NParams  int
	Variadic bool // the last parameter receives a list of the remaining arguments
	NLocals  int  // including parameters
	NTemps   int
	Code     []Instr
}

// Builtin is a built-in function or operator implementation. Exactly one
// of Fn and Gen is set: Fn for functions that produce at most one
// result, Gen for generators.
type Builtin struct {
	Name string
	Fn   func(vm *VM, args []Value) (Value, bool)
	Gen  func(vm *VM, args []Value) Generator
}

// Generator is a suspended built-in generator: Next produces its next
// result, or false when it has no more.
type Generator interface {
	Next(vm *VM) (Value, bool)
}

// Var is a variable reference produced by an expression, such as a list
// element. Locals and globals are referenced by *Value.
type Var interface {
	Get() Value
	Set(v Value) bool // false if the assignment fails
}

// ListElem is the variable L[i] (0-based index into Elems).
type ListElem struct {
	L *List
	I int
}

func (e ListElem) Get() Value {
	if e.I < len(e.L.Elems) {
		return e.L.Elems[e.I]
	}
	return NullValue
}

func (e ListElem) Set(v Value) bool {
	if e.I < len(e.L.Elems) {
		e.L.Elems[e.I] = v
	}
	return true
}

// Deref returns the value of v if it is a variable, else v.
func Deref(v Value) Value {
	switch x := v.(type) {
	case *Value:
		return *x
	case Var:
		return x.Get()
	}
	return v
}

// TypeName returns the Icon type name of a (dereferenced) value.
func TypeName(v Value) string {
	switch v.(type) {
	case Null:
		return "null"
	case Int:
		return "integer"
	case *String:
		return "string"
	case *List:
		return "list"
	case *Proc, *Builtin:
		return "procedure"
	case *Coexp:
		return "co-expression"
	case *Cset:
		return "cset"
	}
	return "unknown"
}

func itoa(i Int) string { return strconv.FormatInt(int64(i), 10) }
