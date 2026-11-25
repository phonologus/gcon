package vm

import "strconv"

// Substring variables, as iconx's substring trapped variables (tvsubs).
// s[i], s[i:j], ?s and !s produce a SubstrVar when s is a variable
// holding a string, so that assigning to the result replaces that part
// of the string: s[2] := "xyz" assigns to s. A substring of a substring
// refers to the original variable, so s[2:5][1] := "q" also assigns to
// s. The positions are bytes, as everywhere (docs/design.md, "Positions
// are bytes").
type SubstrVar struct {
	v      Value // the variable: *Value or a Var other than *SubstrVar
	pos, n int   // 1-based byte position and length
}

// newSubstr returns the substring variable for bytes pos..pos+n-1 of the
// string in variable v.
func newSubstr(v Value, pos, n int) *SubstrVar {
	if s, ok := v.Any().(*SubstrVar); ok {
		return &SubstrVar{s.v, s.pos + pos - 1, n}
	}
	return &SubstrVar{v, pos, n}
}

// strVar reports whether x, an operand passed as a variable, is a
// variable holding a string (not merely convertible to one): only then
// do subscripts produce substring variables.
func strVar(x Value) bool {
	switch x.Any().(type) {
	case *Value, Var:
		_, ok := Deref(x).Any().(*String)
		return ok
	}
	return false
}

// str returns the string the variable holds, which must still be a
// string long enough to contain the substring (errors 103 and 205).
func (s *SubstrVar) str() *String {
	d, ok := Deref(s.v).Any().(*String)
	if !ok {
		Runerr(103, Deref(s.v))
	}
	if s.pos-1+s.n > d.Len() {
		Runerr(205)
	}
	return d
}

func (s *SubstrVar) Get() Value {
	d := s.str()
	return V(d.Slice(s.pos, s.pos+s.n))
}

// Set assigns x to the substring, as iconx's subs_asgn: the variable gets
// the string with the substring replaced, and the substring variable then
// covers the new part.
func (s *SubstrVar) Set(x Value) bool {
	src, ok := CvStr(x)
	if !ok {
		Runerr(103, x)
	}
	d := s.str()
	r := Concat(Concat(d.Slice(1, s.pos), src), d.Slice(s.pos+s.n, d.Len()+1))
	if !setVar(s.v, V(r)) {
		return false
	}
	s.n = src.Len()
	return true
}

// image is outimage's form of a substring variable: the string it is in
// (or &subject), "[i]" or "[i+:n]", and " = " and the substring if it is
// still in range.
func (s *SubstrVar) image(vm *VM, noimage int) string {
	var b string
	if _, isSubj := s.v.Any().(subjectVar); isSubj {
		b = "&subject"
	} else {
		b = vm.OutImage(Deref(s.v), noimage)
	}
	if s.n == 1 {
		b += "[" + strconv.Itoa(s.pos) + "]"
	} else {
		b += "[" + strconv.Itoa(s.pos) + "+:" + strconv.Itoa(s.n) + "]"
	}
	if d, ok := Deref(s.v).Any().(*String); ok && s.pos-1+s.n <= d.Len() {
		b += " = " + vm.OutImage(V(d.Slice(s.pos, s.pos+s.n)), noimage)
	}
	return b
}

// strVarGen is !s for a variable s holding a string: one-character
// substring variables, the string being looked at afresh each time, as
// iconx does.
type strVarGen struct {
	v   Value
	off int // byte offset of the next character
}

func (g *strVarGen) Next(vm *VM) (Value, bool) {
	d, ok := Deref(g.v).Any().(*String)
	if !ok {
		Runerr(103, Deref(g.v))
	}
	if g.off >= d.Len() {
		return Value{}, false
	}
	w := d.width(g.off)
	g.off += w
	return V(newSubstr(g.v, g.off-w+1, w)), true
}
