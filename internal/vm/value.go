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

// Int is an Icon integer that fits in 64 bits; larger ones are *Big
// (bigint.go).
type Int int64

// Unit is the character unit of a string: text (UTF-8, read as runes in
// the manner of Go) or bytes. See docs/design.md, "Strings and Unicode".
type Unit uint8

const (
	TextUnit Unit = iota
	BytesUnit
)

// String is an Icon string: its bytes, its unit, and whether they are
// all ASCII (cached). Strings are immutable.
//
// Positions, subscripts and lengths are byte offsets, as in Go; the unit
// says how characters are read where an operation reads them (!s, move,
// the scanning functions' tests, map, reverse and so on). See
// docs/design.md ("Positions are bytes").
type String struct {
	s     string
	unit  Unit
	ascii int8 // 0 unknown, 1 all ASCII, -1 not
}

// NewString returns a string with the given bytes and unit.
func NewString(s string, u Unit) *String { return &String{s: s, unit: u} }

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

// Len returns the length of s in bytes (*s).
func (s *String) Len() int { return len(s.s) }

// Runes returns the number of characters in s (len(s)): in text, as
// utf8.RuneCountInString counts, each invalid byte being one.
func (s *String) Runes() int {
	if s.unit == BytesUnit || s.IsASCII() {
		return len(s.s)
	}
	return utf8.RuneCountInString(s.s)
}

// Slice returns the substring between byte positions i and j
// (1 <= i <= j <= Len()+1).
func (s *String) Slice(i, j int) *String {
	r := &String{s: s.s[i-1 : j-1], unit: s.unit}
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
	r := &String{s: a.s + b.s, unit: u}
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
	// LocalNames are the names of the parameters and locals, by index
	// (for name() and variable()).
	LocalNames []string
	// Statics are the global slots of the procedure's statics, in
	// declaration order.
	Statics []int
	// Files are the other files code of the procedure came from, by
	// $include (see Instr.File).
	Files  []string
	NTemps int
	Code   []Instr
}

// Builtin is a built-in function or operator implementation. Exactly one
// of Fn and Gen is set: Fn for functions that produce at most one
// result, Gen for generators.
type Builtin struct {
	Name string
	Fn   func(vm *VM, args []Value) (Value, bool)
	Gen  func(vm *VM, args []Value) Generator
	// NoDeref: the arguments are passed as variables (the assignment
	// operators, invoked by name).
	NoDeref bool
	// RefFirst: the first argument is passed as a variable (subscripts,
	// sections, ?x and !x, which make substring variables of strings).
	RefFirst bool
	// intOp, if not zero, names a binary operator the VM computes
	// itself when both operands are Ints (see fastInt).
	intOp intOp
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
	switch x := v.(type) {
	case Null:
		return "null"
	case Int, *Big:
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
	case Real:
		return "real"
	case *Record:
		return x.T.Name
	case *RecordType:
		return "procedure"
	case *File:
		return "file"
	case *Table:
		return "table"
	case *Set:
		return "set"
	}
	return "unknown"
}

func itoa(i Int) string { return strconv.FormatInt(int64(i), 10) }

// FileAt returns the source file of the instruction at pc.
func (p *Proc) FileAt(pc int) string {
	if pc >= 0 && pc < len(p.Code) {
		if k := p.Code[pc].File; k > 0 {
			return p.Files[k-1]
		}
	}
	return p.File
}
