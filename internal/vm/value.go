package vm

import (
	"strconv"
	"unicode/utf8"
)

// Null is &null in the representation V and Value.Any use.
type Null struct{}

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
// all ASCII. Strings are immutable and passed by value; in a Value, a
// string is a pointer to its bytes with its length, unit and ASCII flag
// (valuecore.go), so making a substring or a string Value allocates
// nothing, as in iconx, whose strings are a length and a pointer into
// its string region.
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

// NewString returns a string with the given bytes and unit. Making a
// string reads its bytes anyway, so whether they are ASCII is found now.
func NewString(s string, u Unit) String {
	return String{s: s, unit: u, ascii: asciiFlag(s)}
}

// Text returns a text string.
func Text(s string) String { return NewString(s, TextUnit) }

// asciiFlag returns 1 if every byte of s is ASCII, else -1.
func asciiFlag(s string) int8 {
	for i := 0; i < len(s); i++ {
		if s[i] >= utf8.RuneSelf {
			return -1
		}
	}
	return 1
}

// Go returns the string's bytes.
func (s String) Go() string { return s.s }

// IsASCII reports whether every byte of s is ASCII.
func (s String) IsASCII() bool {
	if s.ascii == 0 {
		return asciiFlag(s.s) > 0
	}
	return s.ascii > 0
}

// withASCII returns s with its ASCII flag known.
func (s String) withASCII() String {
	if s.ascii == 0 {
		s.ascii = asciiFlag(s.s)
	}
	return s
}

// IsBytes reports whether s counts in bytes. ASCII strings behave the
// same in either unit.
func (s String) IsBytes() bool { return s.unit == BytesUnit }

// Unit returns the string's unit.
func (s String) Unit() Unit { return s.unit }

// Len returns the length of s in bytes (*s).
func (s String) Len() int { return len(s.s) }

// Runes returns the number of characters in s (len(s)): in text, as
// utf8.RuneCountInString counts, each invalid byte being one.
func (s String) Runes() int {
	if s.unit == BytesUnit || s.IsASCII() {
		return len(s.s)
	}
	return utf8.RuneCountInString(s.s)
}

// Slice returns the substring between byte positions i and j
// (1 <= i <= j <= Len()+1). A substring of ASCII is ASCII; otherwise
// whether it is is left to be found when needed.
func (s String) Slice(i, j int) String {
	r := String{s: s.s[i-1 : j-1], unit: s.unit}
	if s.ascii > 0 {
		r.ascii = 1
	}
	return r
}

// Concat joins a and b. The result is text if either operand is
// non-ASCII text, else bytes if either is non-ASCII bytes.
func Concat(a, b String) String {
	a, b = a.withASCII(), b.withASCII()
	u := TextUnit
	switch {
	case a.unit == TextUnit && a.ascii < 0, b.unit == TextUnit && b.ascii < 0:
	case a.ascii < 0 || b.ascii < 0:
		u = BytesUnit
	}
	r := String{s: a.s + b.s, unit: u, ascii: -1}
	if a.ascii > 0 && b.ascii > 0 {
		r.ascii = 1
	}
	return r
}

// List is an Icon list.
type List struct {
	Elems []Value
	// base is the array Elems was last placed in by pushFront, with
	// room before it; pushFront reuses that room while Elems still lies
	// in base (a put may have moved it).
	base   []Value
	serial int
}

// pushFront puts v at the front of l: in the room before Elems when
// there is some (left by get and pop, or made by an earlier push), else
// in a new array with room at both ends, so that a run of pushes and
// pops allocates only now and then.
func (l *List) pushFront(v Value) {
	if b := l.base; len(l.Elems) > 0 && cap(l.Elems) <= cap(b) {
		h := cap(b) - cap(l.Elems)
		b = b[:cap(b)]
		if h > 0 && &b[h] == &l.Elems[0] {
			b[h-1] = v
			l.Elems = b[h-1 : h+len(l.Elems)]
			return
		}
	}
	n := len(l.Elems)
	room := n + 4
	b := make([]Value, room+n+n/2+4)
	copy(b[room:], l.Elems)
	b[room-1] = v
	l.Elems, l.base = b[room-1:room+n], b
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
	// Restore, if set, is Gen for a builtin that produces one value
	// and, resumed, restores &pos and fails (see restoring): the value,
	// the old &pos, and whether it succeeds.
	Restore func(vm *VM, args []Value) (Value, int, bool)
	// NoDeref: the arguments are passed as variables (the assignment
	// operators, invoked by name).
	NoDeref bool
	// RefFirst: the first argument is passed as a variable (subscripts,
	// sections, ?x and !x, which make substring variables of strings).
	RefFirst bool
	// intOp, if not zero, names a binary operator the VM computes
	// itself when both operands are Ints (see fastInt).
	intOp intOp
	// How a call written in the program adjusts the arguments
	// (adjustArgs), worked out once for the builtins of the tables.
	adjMode uint8
	adjN    int32
	adjSet  bool
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

// TypeName returns the Icon type name of a (dereferenced) value.
func TypeName(v Value) string {
	switch v.Kind() {
	case KNull:
		return "null"
	case KInt, KBig:
		return "integer"
	case KString:
		return "string"
	case KList:
		return "list"
	case KProc, KBuiltin:
		return "procedure"
	case KCoexp:
		return "co-expression"
	case KCset:
		return "cset"
	case KReal:
		return "real"
	case KRecord:
		x := v.Record()
		return x.T.Name
	case KRecordType:
		return "procedure"
	case KFile:
		return "file"
	case KTable:
		return "table"
	case KSet:
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
