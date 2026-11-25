package vm

import (
	"math/bits"
	"strconv"
	"strings"
	"unicode/utf8"
)

// errorText is iconx's table of run-time error messages
// (src/runtime/data.r, public domain).
var errorText = map[int]string{
	101: "integer expected or out of range",
	102: "numeric expected",
	103: "string expected",
	104: "cset expected",
	105: "file expected",
	106: "procedure or integer expected",
	107: "record expected",
	108: "list expected",
	109: "string or file expected",
	110: "string or list expected",
	111: "variable expected",
	112: "invalid type to size operation",
	113: "invalid type to random operation",
	114: "invalid type to subscript operation",
	115: "structure expected",
	116: "invalid type to element generator",
	117: "missing main procedure",
	118: "co-expression expected",
	119: "set expected",
	120: "two csets or two sets expected",
	121: "function not supported",
	122: "set or table expected",
	123: "invalid type",
	124: "table expected",
	125: "list, record, or set expected",
	126: "list or record expected",
	201: "division by zero",
	202: "remaindering by zero",
	203: "integer overflow",
	204: "real overflow, underflow, or division by zero",
	205: "invalid value",
	206: "negative first argument to real exponentiation",
	207: "invalid field name",
	208: "second and third arguments to map of unequal length",
	209: "invalid second argument to open",
	210: "non-ascending arguments to detab/entab",
	211: "by value equal to zero",
	212: "attempt to read file not open for reading",
	213: "attempt to write file not open for writing",
	214: "input/output error",
	215: "attempt to refresh &main",
	216: "external function not found",
	500: "program malfunction",
}

// ---------------------------------------------------------------------
// Conversions

// CvInt converts v to an integer, if it can.
func CvInt(v Value) (Int, bool) {
	n, ok := CvNum(v)
	if !ok {
		return 0, false
	}
	if r, isReal := n.(Real); isReal {
		return realToInt(float64(r))
	}
	return n.(Int), true
}

// CvStr converts v to a string, if it can.
func CvStr(v Value) (*String, bool) {
	switch x := v.(type) {
	case *String:
		return x, true
	case Int:
		return &String{s: itoa(x), n: -1, ascii: 1}, true
	case Real:
		return &String{s: rtos(float64(x)), n: -1, ascii: 1}, true
	case *Cset:
		return x.String(), true
	}
	return nil, false
}

// MustInt converts v to an integer or raises error 101.
func MustInt(v Value) Int {
	i, ok := CvInt(v)
	if !ok {
		Runerr(101, v)
	}
	return i
}

// MustStr converts v to a string or raises error 103.
func MustStr(v Value) *String {
	s, ok := CvStr(v)
	if !ok {
		Runerr(103, v)
	}
	return s
}

// cvpos converts an Icon position to 1..n+1 for a sequence of length n.
func cvpos(p Int, n int) (int, bool) {
	if p <= 0 {
		p += Int(n) + 1
	}
	if p < 1 || p > Int(n)+1 {
		return 0, false
	}
	return int(p), true
}

// ---------------------------------------------------------------------
// Operators

func overflow() Int { Runerr(203); return 0 }

func add(x, y Int) Int {
	s := x + y
	if (s > x) != (y > 0) {
		return overflow()
	}
	return s
}

func sub(x, y Int) Int {
	d := x - y
	if (d < x) != (y > 0) {
		return overflow()
	}
	return d
}

func mul(x, y Int) Int {
	if x == 0 || y == 0 {
		return 0
	}
	hi, lo := bits.Mul64(uint64(abs(x)), uint64(abs(y)))
	if hi != 0 || lo > 1<<63-1 {
		if hi == 0 && lo == 1<<63 && (x < 0) != (y < 0) {
			return -1 << 63
		}
		return overflow()
	}
	r := Int(lo)
	if (x < 0) != (y < 0) {
		r = -r
	}
	return r
}

func abs(x Int) Int {
	if x < 0 {
		return -x
	}
	return x
}

func div(x, y Int) Int {
	if y == 0 {
		Runerr(201)
	}
	if x == -1<<63 && y == -1 {
		return overflow()
	}
	return x / y
}

func mod(x, y Int) Int {
	if y == 0 {
		Runerr(202)
	}
	if y == -1 {
		return 0
	}
	return x % y
}

func pow(x, y Int) Int {
	if y < 0 {
		switch {
		case x == 0:
			Runerr(204)
		case x == 1:
			return 1
		case x == -1:
			if y%2 == 0 {
				return 1
			}
			return -1
		}
		return 0
	}
	r := Int(1)
	for ; y > 0; y-- {
		r = mul(r, x)
	}
	return r
}

func strCompare(name string, ok func(c int) bool) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := MustStr(a[0]), MustStr(a[1])
		return y, ok(strings.Compare(x.s, y.s))
	}}
}

// Identical reports whether x === y.
func Identical(x, y Value) bool {
	switch a := x.(type) {
	case Null:
		_, ok := y.(Null)
		return ok
	case Int:
		b, ok := y.(Int)
		return ok && a == b
	case Real:
		b, ok := y.(Real)
		return ok && a == b
	case *String:
		b, ok := y.(*String)
		return ok && a.s == b.s && (a.unit == b.unit || a.IsASCII())
	case *Cset:
		b, ok := y.(*Cset)
		return ok && equalCsets(a, b)
	}
	return x == y
}

// Ops holds the operator implementations, by operator spelling; unary
// operators are prefixed with "u".
var Ops = map[string]*Builtin{
	"+": arith("+", add, func(x, y float64) float64 { return x + y }),
	"-": arith("-", sub, func(x, y float64) float64 { return x - y }),
	"*": arith("*", mul, func(x, y float64) float64 { return x * y }),
	"/": arith("/", div, rdiv),
	"%": arith("%", mod, rmod),
	"^": {Name: "^", Fn: power},

	"<":  numCompare("<", func(c int) bool { return c < 0 }),
	"<=": numCompare("<=", func(c int) bool { return c <= 0 }),
	">":  numCompare(">", func(c int) bool { return c > 0 }),
	">=": numCompare(">=", func(c int) bool { return c >= 0 }),
	"=":  numCompare("=", func(c int) bool { return c == 0 }),
	"~=": numCompare("~=", func(c int) bool { return c != 0 }),

	"==":  strCompare("==", func(c int) bool { return c == 0 }),
	"~==": strCompare("~==", func(c int) bool { return c != 0 }),
	"<<":  strCompare("<<", func(c int) bool { return c < 0 }),
	"<<=": strCompare("<<=", func(c int) bool { return c <= 0 }),
	">>":  strCompare(">>", func(c int) bool { return c > 0 }),
	">>=": strCompare(">>=", func(c int) bool { return c >= 0 }),

	"===": {Name: "===", Fn: func(vm *VM, a []Value) (Value, bool) {
		return a[1], Identical(a[0], a[1])
	}},
	"~===": {Name: "~===", Fn: func(vm *VM, a []Value) (Value, bool) {
		return a[1], !Identical(a[0], a[1])
	}},

	"||": {Name: "||", Fn: func(vm *VM, a []Value) (Value, bool) {
		return Concat(MustStr(a[0]), MustStr(a[1])), true
	}},
	"|||": {Name: "|||", Fn: func(vm *VM, a []Value) (Value, bool) {
		x, ok1 := a[0].(*List)
		y, ok2 := a[1].(*List)
		if !ok1 {
			Runerr(108, a[0])
		}
		if !ok2 {
			Runerr(108, a[1])
		}
		e := make([]Value, 0, len(x.Elems)+len(y.Elems))
		return vm.NewList(append(append(e, x.Elems...), y.Elems...)), true
	}},

	"u-": {Name: "-", Fn: func(vm *VM, a []Value) (Value, bool) {
		if r, ok := MustNum(a[0]).(Real); ok {
			return -r, true
		}
		return sub(0, MustNum(a[0]).(Int)), true
	}},
	"u+": {Name: "+", Fn: func(vm *VM, a []Value) (Value, bool) {
		return MustNum(a[0]), true
	}},
	"u*": {Name: "*", Fn: func(vm *VM, a []Value) (Value, bool) {
		switch x := a[0].(type) {
		case *List:
			return Int(len(x.Elems)), true
		case *Coexp:
			return Int(x.results), true
		case *Cset:
			return Int(x.Size()), true
		case *Record:
			return Int(len(x.Fields)), true
		}
		if s, ok := CvStr(a[0]); ok {
			return Int(s.Len()), true
		}
		Runerr(112, a[0])
		return nil, false
	}},
	"u!": {Name: "!", Gen: bang},

	"[]":  {Name: "[]", Fn: index},
	"[:]": {Name: "[:]", Fn: section},
	"[+:]": {Name: "[+:]", Fn: func(vm *VM, a []Value) (Value, bool) {
		i := MustInt(a[1])
		return section(vm, []Value{a[0], i, add(i, MustInt(a[2]))})
	}},
	"[-:]": {Name: "[-:]", Fn: func(vm *VM, a []Value) (Value, bool) {
		i := MustInt(a[1])
		return section(vm, []Value{a[0], i, sub(i, MustInt(a[2]))})
	}},
	"[...]": {Name: "[...]", Fn: func(vm *VM, a []Value) (Value, bool) {
		return vm.NewList(append([]Value(nil), a...)), true
	}},
	"to": {Name: "to", Gen: toBy},
}

func index(vm *VM, a []Value) (Value, bool) {
	switch x := a[0].(type) {
	case *Record:
		return recordIndex(x, a[1])
	case *List:
		i := MustInt(a[1])
		n := Int(len(x.Elems))
		if i <= 0 {
			i += n + 1
		}
		if i < 1 || i > n {
			return nil, false
		}
		return ListElem{x, int(i - 1)}, true
	}
	// TODO: subscripts of string variables are assignable in Icon.
	s, ok := CvStr(a[0])
	if !ok {
		Runerr(114, a[0])
	}
	i, ok := cvpos(MustInt(a[1]), s.Len())
	if !ok || i > s.Len() {
		return nil, false
	}
	return s.Slice(i, i+1), true
}

func section(vm *VM, a []Value) (Value, bool) {
	var n int
	l, isList := a[0].(*List)
	var s *String
	if isList {
		n = len(l.Elems)
	} else {
		var ok bool
		if s, ok = CvStr(a[0]); !ok {
			Runerr(114, a[0])
		}
		n = s.Len()
	}
	i, ok1 := cvpos(MustInt(a[1]), n)
	j, ok2 := cvpos(MustInt(a[2]), n)
	if !ok1 || !ok2 {
		return nil, false
	}
	if i > j {
		i, j = j, i
	}
	if isList {
		return vm.NewList(append([]Value(nil), l.Elems[i-1:j-1]...)), true
	}
	return s.Slice(i, j), true
}

// toBy generates i to j by k.
func toBy(vm *VM, a []Value) Generator {
	// As in iconx, the operands are integers; reals are truncated.
	g := &toGen{i: MustInt(a[0]), j: MustInt(a[1]), by: 1}
	if len(a) > 2 {
		g.by = MustInt(a[2])
	}
	if g.by == 0 {
		Runerr(211, g.by)
	}
	return g
}

type toGen struct {
	i, j, by Int
	started  bool
}

func (g *toGen) Next(vm *VM) (Value, bool) {
	if g.started {
		n := g.i + g.by
		if (n < g.i) != (g.by < 0) { // overflow: the sequence has ended
			return nil, false
		}
		g.i = n
	}
	g.started = true
	if (g.by > 0 && g.i > g.j) || (g.by < 0 && g.i < g.j) {
		return nil, false
	}
	return g.i, true
}

// bang generates the elements of a list, or the characters of a string.
func bang(vm *VM, a []Value) Generator {
	switch x := a[0].(type) {
	case *List:
		return &listGen{l: x}
	case *Record:
		return &recordGen{r: x}
	case *File:
		return lineGen{x}
	}
	s, ok := CvStr(a[0])
	if !ok {
		Runerr(116, a[0])
	}
	return &strGen{s: s}
}

type listGen struct {
	l *List
	i int
}

func (g *listGen) Next(vm *VM) (Value, bool) {
	if g.i >= len(g.l.Elems) {
		return nil, false
	}
	g.i++
	return ListElem{g.l, g.i - 1}, true
}

type strGen struct {
	s *String
	i int // characters produced
}

func (g *strGen) Next(vm *VM) (Value, bool) {
	if g.i >= g.s.Len() {
		return nil, false
	}
	g.i++
	return g.s.Slice(g.i, g.i+1), true
}

// ---------------------------------------------------------------------
// image()

// Image returns the image of v, as image(v).
func (vm *VM) Image(v Value) string {
	switch x := Deref(v).(type) {
	case Null:
		return "&null"
	case Int:
		return itoa(x)
	case *String:
		return quote(x, '"')
	case *List:
		return "list_" + strconv.Itoa(x.serial) + "(" + strconv.Itoa(len(x.Elems)) + ")"
	case *Proc:
		return "procedure " + x.Name
	case *Builtin:
		return "function " + x.Name
	case *Coexp:
		return "co-expression_" + strconv.Itoa(x.serial) + "(" + strconv.Itoa(x.results) + ")"
	case *Cset:
		return imageCset(x)
	case Real:
		return rtos(float64(x))
	case *Record:
		return x.image()
	case *RecordType:
		return "record constructor " + x.Name
	case *File:
		return x.image()
	}
	return "?"
}

// quote renders s between q quotes. Non-ASCII text follows Go's %q
// (strconv.Quote): printable characters as themselves, others as \u or
// \U escapes, invalid bytes as \xNN. ASCII follows iconx, which uses
// \b \t \n \v \f \r \e \d and otherwise \xNN for control characters. A
// bytes string escapes every byte from 0x80 as \xNN.
func quote(s *String, q byte) string {
	var b strings.Builder
	b.WriteByte(q)
	str := s.s
	for i := 0; i < len(str); {
		c := str[i]
		if c < utf8.RuneSelf || s.unit == BytesUnit {
			quoteByte(&b, c, q)
			i++
			continue
		}
		r, w := utf8.DecodeRuneInString(str[i:])
		switch {
		case r == utf8.RuneError && w == 1:
			quoteByte(&b, c, q)
		case strconv.IsPrint(r):
			b.WriteString(str[i : i+w])
		case r < 0x10000:
			b.WriteString(`\u`)
			b.WriteString(hex4(int(r)))
		default:
			// Always six digits: \U reads up to six, so a shorter escape
			// followed by a hex digit would read back differently.
			h := strconv.FormatInt(int64(r), 16)
			b.WriteString(`\U`)
			b.WriteString(strings.Repeat("0", 6-len(h)) + h)
		}
		i += w
	}
	b.WriteByte(q)
	return b.String()
}

func hex4(v int) string {
	h := strconv.FormatInt(int64(v), 16)
	return strings.Repeat("0", 4-len(h)) + h
}

func quoteByte(b *strings.Builder, c byte, q byte) {
	switch {
	case c == q || c == '\\':
		b.WriteByte('\\')
		b.WriteByte(c)
	case c == '\b':
		b.WriteString(`\b`)
	case c == '\t':
		b.WriteString(`\t`)
	case c == '\n':
		b.WriteString(`\n`)
	case c == '\v':
		b.WriteString(`\v`)
	case c == '\f':
		b.WriteString(`\f`)
	case c == '\r':
		b.WriteString(`\r`)
	case c == 033:
		b.WriteString(`\e`)
	case c == 0177:
		b.WriteString(`\d`)
	case c < ' ' || c >= 0x80:
		b.WriteString(`\x`)
		h := strconv.FormatInt(int64(c), 16)
		if len(h) < 2 {
			b.WriteByte('0')
		}
		b.WriteString(h)
	default:
		b.WriteByte(c)
	}
}
