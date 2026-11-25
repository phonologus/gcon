package vm

import (
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
	131: "external expected",
	132: "incorrect external type",
	133: "invalid external value",
	134: "malformed external value",
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
	300: "interrupted",
	301: "evaluation stack overflow",
	302: "memory violation",
	303: "inadequate space for evaluation stack",
	304: "inadequate space in qualifier list",
	305: "inadequate space for static allocation",
	306: "inadequate space in string region",
	307: "inadequate space in block region",
	308: "system stack overflow in co-expression",
	402: "program not compiled with debugging option",
	500: "program malfunction",
}

// ---------------------------------------------------------------------
// Conversions

// CvInt converts v to a word-sized integer (iconx's cnv:C_integer), if
// it can; a large integer cannot be. CvInteger allows large ones.
func CvInt(v Value) (Int, bool) {
	n, ok := CvNum(v)
	if !ok {
		return 0, false
	}
	switch n.Kind() {
	case KReal:
		x := n.Real()
		return realToInt(float64(x))
	case KInt:
		x := n.Int()
		return x, true
	}
	return 0, false // a large integer is out of range
}

// CvStr converts v to a string, if it can.
func CvStr(v Value) (*String, bool) {
	switch v.Kind() {
	case KString:
		x := v.Str()
		return x, true
	case KInt:
		x := v.Int()
		return &String{s: itoa(x), ascii: 1}, true
	case KBig:
		x := v.Big()
		return &String{s: x.v.String(), ascii: 1}, true
	case KReal:
		x := v.Real()
		return &String{s: rtos(float64(x)), ascii: 1}, true
	case KCset:
		x := v.Cset()
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

// add is word-sized addition where iconx raises error 203 on overflow
// rather than making a large integer (seq).
func add(x, y Int) Int {
	s, ok := addInt(x, y)
	if !ok {
		Runerr(203)
	}
	return s
}

func strCompare(name string, ok func(c int) bool) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := MustStr(a[0]), MustStr(a[1])
		return StrVal(y), ok(strings.Compare(x.s, y.s))
	}}
}

// Identical reports whether x === y.
func Identical(x, y Value) bool {
	switch x.Kind() {
	case KNull:
		_, ok := y.AsNull()
		return ok
	case KInt:
		a := x.Int()
		b, ok := y.AsInt()
		return ok && a == b
	case KBig:
		a := x.Big()
		b, ok := y.AsBig()
		return ok && a.v.Cmp(b.v) == 0
	case KReal:
		a := x.Real()
		b, ok := y.AsReal()
		return ok && a == b
	case KString:
		a := x.Str()
		b, ok := y.AsString()
		return ok && a.s == b.s && (a.unit == b.unit || a.IsASCII())
	case KCset:
		a := x.Cset()
		b, ok := y.AsCset()
		return ok && equalCsets(a, b)
	}
	return x == y
}

// Ops holds the operator implementations, by operator spelling; unary
// operators are prefixed with "u".
var Ops = map[string]*Builtin{
	"+": arith("+", addInt, addBig, func(x, y float64) float64 { return x + y }),
	"-": arith("-", subInt, subBig, func(x, y float64) float64 { return x - y }),
	"*": arith("*", mulInt, mulBig, func(x, y float64) float64 { return x * y }),
	"/": arith("/", divInt, divBig, rdiv),
	"%": arith("%", modInt, modBig, rmod),
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
		return StrVal(Concat(MustStr(a[0]), MustStr(a[1]))), true
	}},
	"|||": {Name: "|||", Fn: func(vm *VM, a []Value) (Value, bool) {
		x, ok1 := a[0].AsList()
		y, ok2 := a[1].AsList()
		if !ok1 {
			Runerr(108, a[0])
		}
		if !ok2 {
			Runerr(108, a[1])
		}
		e := make([]Value, 0, len(x.Elems)+len(y.Elems))
		return ListVal(vm.NewList(append(append(e, x.Elems...), y.Elems...))), true
	}},

	"u-": {Name: "-", Fn: func(vm *VM, a []Value) (Value, bool) {
		if r, ok := MustNum(a[0]).AsReal(); ok {
			return RealVal(-r), true
		}
		return neg(MustNum(a[0])), true
	}},
	"u+": {Name: "+", Fn: func(vm *VM, a []Value) (Value, bool) {
		return MustNum(a[0]), true
	}},
	"u*": {Name: "*", Fn: func(vm *VM, a []Value) (Value, bool) {
		switch val := a[0]; val.Kind() {
		case KList:
			x := val.List()
			return IntVal(Int(len(x.Elems))), true
		case KCoexp:
			x := val.Coexp()
			return IntVal(Int(x.results)), true
		case KCset:
			x := val.Cset()
			return IntVal(Int(x.Size())), true
		case KRecord:
			x := val.Record()
			return IntVal(Int(len(x.Fields))), true
		case KTable:
			x := val.Table()
			return IntVal(Int(x.m.len())), true
		case KSet:
			x := val.Set()
			return IntVal(Int(x.m.len())), true
		}
		if s, ok := CvStr(a[0]); ok {
			return IntVal(Int(s.Len())), true
		}
		Runerr(112, a[0])
		return Value{}, false
	}},
	"u!": {Name: "!", Gen: bang, RefFirst: true},

	"[]":  {Name: "[]", Fn: index, RefFirst: true},
	"[:]": {Name: "[:]", Fn: section, RefFirst: true},
	"[...]": {Name: "[...]", Fn: func(vm *VM, a []Value) (Value, bool) {
		return ListVal(vm.NewList(append([]Value(nil), a...))), true
	}},
	"to": {Name: "to", Gen: toBy},
	// limit validates the limit of e \ n: an integer, at least 0; a limit
	// of 0 fails, so that e produces nothing.
	"limit": {Name: "limit", Fn: func(vm *VM, a []Value) (Value, bool) {
		n := MustInt(a[0])
		if n < 0 {
			Runerr(205, IntVal(n))
		}
		return IntVal(n), n > 0
	}},
}

func index(vm *VM, a []Value) (Value, bool) {
	ref := a[0]
	switch val := Deref(ref); val.Kind() {
	case KRecord:
		x := val.Record()
		return recordIndex(x, a[1])
	case KTable:
		x := val.Table()
		return varVal(TableElem{x, a[1]}), true
	case KList:
		x := val.List()
		i := MustInt(a[1])
		n := Int(len(x.Elems))
		if i <= 0 {
			i += n + 1
		}
		if i < 1 || i > n {
			return Value{}, false
		}
		return listElemVal(x, int(i-1)), true
	}
	s, ok := CvStr(Deref(ref))
	if !ok {
		Runerr(114, Deref(ref))
	}
	i, ok := cvpos(MustInt(a[1]), s.Len())
	if !ok || i > s.Len() {
		return Value{}, false
	}
	if strVar(ref) {
		return substrVal(newSubstr(ref, i, 1)), true
	}
	return StrVal(s.Slice(i, i+1)), true
}

func section(vm *VM, a []Value) (Value, bool) {
	ref := a[0]
	x := Deref(ref)
	var n int
	l, isList := x.AsList()
	var s *String
	if isList {
		n = len(l.Elems)
	} else {
		var ok bool
		if s, ok = CvStr(x); !ok {
			Runerr(110, x)
		}
		n = s.Len()
	}
	i, ok1 := cvpos(MustInt(a[1]), n)
	j, ok2 := cvpos(MustInt(a[2]), n)
	if !ok1 || !ok2 {
		return Value{}, false
	}
	if i > j {
		i, j = j, i
	}
	if isList {
		return ListVal(vm.NewList(append([]Value(nil), l.Elems[i-1:j-1]...))), true
	}
	if strVar(ref) {
		return substrVal(newSubstr(ref, i, j-i)), true
	}
	return StrVal(s.Slice(i, j)), true
}

// toBy generates i to j by k.
func toBy(vm *VM, a []Value) Generator {
	// As in iconx, the operands are integers; reals are truncated.
	g := &toGen{i: MustInt(a[0]), j: MustInt(a[1]), by: 1}
	if len(a) > 2 {
		g.by = MustInt(a[2])
	}
	if g.by == 0 {
		Runerr(211, IntVal(g.by))
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
			return Value{}, false
		}
		g.i = n
	}
	g.started = true
	if (g.by > 0 && g.i > g.j) || (g.by < 0 && g.i < g.j) {
		return Value{}, false
	}
	return mkInt(g.i), true
}

// bang generates the elements of a list, or the characters of a string.
func bang(vm *VM, a []Value) Generator {
	if strVar(a[0]) {
		return &strVarGen{v: a[0]}
	}
	switch val := Deref(a[0]); val.Kind() {
	case KList:
		x := val.List()
		return &listGen{l: x}
	case KRecord:
		x := val.Record()
		return &recordGen{r: x}
	case KFile:
		x := val.File()
		return lineGen{x}
	case KTable:
		x := val.Table()
		return tableVals(x)
	case KSet:
		x := val.Set()
		return setElems(x)
	}
	s, ok := CvStr(Deref(a[0]))
	if !ok {
		Runerr(116, Deref(a[0]))
	}
	return &strGen{s: s}
}

// strGen generates the characters of a string (!s), as Go's range does.

type listGen struct {
	l *List
	i int
}

func (g *listGen) Next(vm *VM) (Value, bool) {
	if g.i >= len(g.l.Elems) {
		return Value{}, false
	}
	g.i++
	return listElemVal(g.l, g.i-1), true
}

type strGen struct {
	s   *String
	off int // byte offset of the next character
}

func (g *strGen) Next(vm *VM) (Value, bool) {
	if g.off >= len(g.s.s) {
		return Value{}, false
	}
	w := g.s.width(g.off)
	g.off += w
	return StrVal(g.s.Slice(g.off-w+1, g.off+1)), true
}

// ---------------------------------------------------------------------
// image()

// Image returns the image of v, as image(v).
func (vm *VM) Image(v Value) string {
	switch val := Deref(v); val.Kind() {
	case KNull:
		return "&null"
	case KInt:
		x := val.Int()
		return itoa(x)
	case KBig:
		x := val.Big()
		return bigImage(x.v)
	case KString:
		x := val.Str()
		return quote(x, '"')
	case KList:
		x := val.List()
		return "list_" + strconv.Itoa(x.serial) + "(" + strconv.Itoa(len(x.Elems)) + ")"
	case KProc:
		x := val.Proc()
		return "procedure " + x.Name
	case KBuiltin:
		x := val.Builtin()
		return "function " + x.Name
	case KCoexp:
		x := val.Coexp()
		return "co-expression_" + strconv.Itoa(x.serial) + "(" + strconv.Itoa(x.results) + ")"
	case KCset:
		x := val.Cset()
		return imageCset(x)
	case KReal:
		x := val.Real()
		return rtos(float64(x))
	case KRecord:
		x := val.Record()
		return x.image()
	case KRecordType:
		x := val.RecordType()
		return "record constructor " + x.Name
	case KFile:
		x := val.File()
		return x.image()
	case KTable:
		x := val.Table()
		return x.image()
	case KSet:
		x := val.Set()
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
