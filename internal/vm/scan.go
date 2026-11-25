package vm

import (
	"strings"
	"unicode/utf8"
)

// String scanning. &subject and &pos are global, as in iconx: switching
// co-expressions does not switch them.
//
// Positions are byte positions (docs/design.md, "Positions are bytes").
// A function that tests or steps over characters (move, upto, many, any,
// bal) reads the character at a position and steps by its width; match,
// find and =s compare bytes, as Go's strings.HasPrefix and strings.Index
// do.

// scanEnv is a saved scanning environment, held in a temporary by
// OpScanEnter and exchanged with the current one by OpScanSwap.
type scanEnv struct {
	subject *String
	pos     int
}

// scanEnter saves the current environment in a new scanEnv and starts
// scanning v.
func (vm *VM) scanEnter(v Value) *scanEnv {
	s, ok := CvStr(v)
	if !ok {
		Runerr(103, v)
	}
	e := &scanEnv{vm.subject, vm.pos}
	vm.subject, vm.pos = s, 1
	return e
}

// scanSwap exchanges the current environment with e.
func (vm *VM) scanSwap(e *scanEnv) {
	e.subject, vm.subject = vm.subject, e.subject
	e.pos, vm.pos = vm.pos, e.pos
}

// subjectVar is the variable &subject.
type subjectVar struct{ vm *VM }

func (k subjectVar) Get() Value { return k.vm.subject }
func (k subjectVar) Set(v Value) bool {
	k.vm.subject, k.vm.pos = MustStr(v), 1
	return true
}

// posVar is the variable &pos; assigning an invalid position fails.
type posVar struct{ vm *VM }

func (k posVar) Get() Value { return Int(k.vm.pos) }
func (k posVar) Set(v Value) bool {
	p, ok := cvpos(MustInt(v), k.vm.subject.Len())
	if ok {
		k.vm.pos = p
	}
	return ok
}

// width returns the width in bytes of the character at byte offset i.
func (s *String) width(i int) int {
	if s.unit == BytesUnit || s.s[i] < utf8.RuneSelf {
		return 1
	}
	_, w := utf8.DecodeRuneInString(s.s[i:])
	return w
}

// widthBefore returns the width in bytes of the character ending at byte
// offset i.
func (s *String) widthBefore(i int) int {
	if s.unit == BytesUnit || s.s[i-1] < utf8.RuneSelf {
		return 1
	}
	_, w := utf8.DecodeLastRuneInString(s.s[:i])
	return w
}

// scanArgs converts the subject and range arguments a[k], a[k+1],
// a[k+2] of a string analysis function as iconx's str_anal does: s
// defaults to &subject, and then i to &pos, else to 1; j defaults to the
// end. It fails if a position is out of range.
func (vm *VM) scanArgs(a []Value, k int) (s *String, i, j int, ok bool) {
	sv, iv, jv := arg(a, k), arg(a, k+1), arg(a, k+2)
	_, iNull := iv.(Null)
	if _, null := sv.(Null); null {
		s = vm.subject
		if iNull {
			i = vm.pos
		}
	} else {
		s = MustStr(sv)
		if iNull {
			i = 1
		}
	}
	n := s.Len()
	if !iNull {
		if i, ok = cvpos(MustInt(iv), n); !ok {
			return nil, 0, 0, false
		}
	}
	if _, null := jv.(Null); null {
		j = n + 1
	} else {
		if j, ok = cvpos(MustInt(jv), n); !ok {
			return nil, 0, 0, false
		}
		if i > j {
			i, j = j, i
		}
	}
	return s, i, j, true
}

// MustCset converts v to a cset or raises error 104.
func MustCset(v Value) *Cset {
	c, ok := CvCset(v)
	if !ok {
		Runerr(104, v)
	}
	return c
}

// CvCset converts v to a cset, if it can.
func CvCset(v Value) (*Cset, bool) {
	if c, ok := v.(*Cset); ok {
		return c, true
	}
	if s, ok := CvStr(v); ok {
		return CsetOf(s), true
	}
	return nil, false
}

// csetArg converts a[i] to a cset, with default d.
func csetArg(a []Value, i int, d *Cset) *Cset {
	if _, null := arg(a, i).(Null); null {
		return d
	}
	return MustCset(a[i])
}

// matchAt reports whether the character of s at byte offset off is in c,
// and its width.
func (c *Cset) matchAt(s *String, off int) (int, bool) {
	if s.unit == BytesUnit {
		return 1, c.MatchByte(s.s[off])
	}
	return c.MatchText(s.s, off)
}

// matches reports whether t's bytes occur in s at byte offset off,
// ending at or before byte offset end.
func matches(s *String, off, end int, t *String) bool {
	n := len(t.s)
	return off+n <= end && s.s[off:off+n] == t.s
}

// restorer is a generator that produces a value once and, when resumed,
// restores &pos and fails, as tab, move and =s do.
type restorer struct {
	v    Value
	old  int
	done bool
}

func (g *restorer) Next(vm *VM) (Value, bool) {
	if g.done {
		vm.pos = g.old
		return nil, false
	}
	g.done = true
	return g.v, true
}

// failGen produces nothing.
type failGen struct{}

func (failGen) Next(*VM) (Value, bool) { return nil, false }

// tabTo moves &pos to p, producing the substring passed over.
func (vm *VM) tabTo(p int) Generator {
	old := vm.pos
	i, j := old, p
	if i > j {
		i, j = j, i
	}
	v := vm.subject.Slice(i, j)
	vm.pos = p
	return &restorer{v: v, old: old}
}

// funcGen is a generator from a Go function returning successive results.
type funcGen func() (Value, bool)

func (f funcGen) Next(*VM) (Value, bool) { return f() }

func gen(name string, f func(vm *VM, a []Value) Generator) {
	Functions[name] = &Builtin{Name: name, Gen: f}
}

func init() {
	Ops["&subject"] = &Builtin{Name: "&subject", Fn: func(vm *VM, a []Value) (Value, bool) {
		return subjectVar{vm}, true
	}}
	Ops["&pos"] = &Builtin{Name: "&pos", Fn: func(vm *VM, a []Value) (Value, bool) {
		return posVar{vm}, true
	}}
	// =s is tab(match(s)).
	Ops["u="] = &Builtin{Name: "=", Gen: func(vm *VM, a []Value) Generator {
		t := MustStr(a[0])
		if !matches(vm.subject, vm.pos-1, len(vm.subject.s), t) {
			return failGen{}
		}
		return vm.tabTo(vm.pos + len(t.s))
	}}
	Ops["u~"] = &Builtin{Name: "~", Fn: func(vm *VM, a []Value) (Value, bool) {
		return Complement(MustCset(a[0])), true
	}}
	for _, op := range []struct {
		name string
		c    byte
	}{{"++", '+'}, {"**", '*'}, {"--", '-'}} {
		c := op.c
		Ops[op.name] = &Builtin{Name: op.name, Fn: func(vm *VM, a []Value) (Value, bool) {
			x, ok1 := CvCset(a[0])
			y, ok2 := CvCset(a[1])
			if !ok1 || !ok2 {
				if !ok1 {
					Runerr(120, a[0])
				}
				Runerr(120, a[1])
			}
			return CsetOp(c, x, y), true
		}}
	}

	fn("cset", func(vm *VM, a []Value) (Value, bool) {
		return CvCset(arg(a, 0))
	})
	gen("tab", func(vm *VM, a []Value) Generator {
		p, ok := cvpos(MustInt(arg(a, 0)), vm.subject.Len())
		if !ok {
			return failGen{}
		}
		return vm.tabTo(p)
	})
	// move(n) moves over n characters (backwards if n is negative).
	gen("move", func(vm *VM, a []Value) Generator {
		n := MustInt(arg(a, 0))
		s, off := vm.subject, vm.pos-1
		for ; n > 0; n-- {
			if off >= len(s.s) {
				return failGen{}
			}
			off += s.width(off)
		}
		for ; n < 0; n++ {
			if off <= 0 {
				return failGen{}
			}
			off -= s.widthBefore(off)
		}
		return vm.tabTo(off + 1)
	})
	fn("pos", func(vm *VM, a []Value) (Value, bool) {
		p, ok := cvpos(MustInt(arg(a, 0)), vm.subject.Len())
		return Int(p), ok && p == vm.pos
	})
	fn("match", func(vm *VM, a []Value) (Value, bool) {
		s, i, j, ok := vm.scanArgs(a, 1)
		if !ok {
			return nil, false
		}
		t := MustStr(arg(a, 0))
		return Int(i + len(t.s)), matches(s, i-1, j-1, t)
	})
	gen("find", func(vm *VM, a []Value) Generator {
		s, i, j, ok := vm.scanArgs(a, 1)
		if !ok {
			return failGen{}
		}
		t := MustStr(arg(a, 0))
		// Every byte position from i to j where t occurs (an empty t
		// occurs at each, j included), as iconx and strings.Index find.
		off, end := i-1, j-1
		return funcGen(func() (Value, bool) {
			if off > end {
				return nil, false
			}
			k := strings.Index(s.s[off:end], t.s)
			if k < 0 {
				off = end + 1
				return nil, false
			}
			p := off + k + 1
			off += k + 1
			return Int(p), true
		})
	})
	gen("upto", func(vm *VM, a []Value) Generator {
		c := MustCset(arg(a, 0))
		s, i, j, ok := vm.scanArgs(a, 1)
		if !ok {
			return failGen{}
		}
		off, end := i-1, j-1
		return funcGen(func() (Value, bool) {
			for off < end {
				w, in := c.matchAt(s, off)
				p := off + 1
				off += w
				if in {
					return Int(p), true
				}
			}
			return nil, false
		})
	})
	fn("many", func(vm *VM, a []Value) (Value, bool) {
		c := MustCset(arg(a, 0))
		s, i, j, ok := vm.scanArgs(a, 1)
		if !ok {
			return nil, false
		}
		off, end := i-1, j-1
		for off < end {
			w, in := c.matchAt(s, off)
			if !in {
				break
			}
			off += w
		}
		return Int(off + 1), off > i-1
	})
	fn("any", func(vm *VM, a []Value) (Value, bool) {
		c := MustCset(arg(a, 0))
		s, i, j, ok := vm.scanArgs(a, 1)
		if !ok || i >= j {
			return nil, false
		}
		w, in := c.matchAt(s, i-1)
		return Int(i + w), in
	})
	gen("bal", func(vm *VM, a []Value) Generator {
		s, i, j, ok := vm.scanArgs(a, 3)
		c1 := csetArg(a, 0, CsetAll)
		c2 := csetArg(a, 1, csetFrom("(", TextUnit))
		c3 := csetArg(a, 2, csetFrom(")", TextUnit))
		if !ok {
			return failGen{}
		}
		off, end, cnt := i-1, j-1, 0
		return funcGen(func() (Value, bool) {
			for off < end {
				o := off
				w, in1 := c1.matchAt(s, o)
				_, in2 := c2.matchAt(s, o)
				_, in3 := c3.matchAt(s, o)
				// Advance past this character before suspending, so
				// that resumption continues from the next one.
				off += w
				suspend := cnt == 0 && in1
				if in2 {
					cnt++
				} else if in3 {
					cnt--
				}
				if suspend {
					if cnt < 0 {
						end = off // fail when resumed
					}
					return Int(o + 1), true
				}
				if cnt < 0 {
					return nil, false
				}
			}
			return nil, false
		})
	})
}
