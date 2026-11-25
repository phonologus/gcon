package vm

import (
	"strings"
	"unicode"
	"unicode/utf8"
)

// String functions, following iconx's fstr.r. They work in characters
// of their strings' units, so that left("é", 3) pads to three characters.

// charsOf returns the characters of s, each as its bytes.
func charsOf(s *String) []string {
	cs := make([]string, 0, len(s.s))
	for i := 0; i < len(s.s); {
		w := s.width(i)
		cs = append(cs, s.s[i:i+w])
		i += w
	}
	return cs
}

// joinUnit returns the unit of a string made of characters from the
// given strings, by the rule for concatenation.
func joinUnit(ss ...*String) Unit {
	u := TextUnit
	for _, s := range ss {
		if !s.IsASCII() {
			if s.unit == TextUnit {
				return TextUnit
			}
			u = BytesUnit
		}
	}
	return u
}

// padArgs converts the arguments of left, right and center: s1, the
// width n (default 1, error 205 if negative), and the pad s2 (default
// and empty: a blank).
func padArgs(a []Value) (*String, int, *String) {
	s1 := MustStr(arg(a, 0))
	n := 1
	if _, null := arg(a, 1).(Null); !null {
		n = int(MustInt(a[1]))
	}
	if n < 0 {
		Runerr(205, Int(n))
	}
	s2 := Text(" ")
	if _, null := arg(a, 2).(Null); !null {
		if s := MustStr(a[2]); s.s != "" {
			s2 = s
		}
	}
	return s1, n, s2
}

func join(cs []string, u Unit) *String { return NewString(strings.Join(cs, ""), u) }

func init() {
	fn("left", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1, c2 := charsOf(s1), charsOf(s2)
		if n <= len(c1) {
			return s1.Slice(1, n+1), true
		}
		// Copies of s2 from the right end, then s1 at the left.
		buf := make([]string, n)
		for i := n; i > 0; {
			for j := len(c2); j > 0 && i > 0; j-- {
				i--
				buf[i] = c2[j-1]
			}
		}
		copy(buf, c1)
		return join(buf, joinUnit(s1, s2)), true
	})
	fn("right", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1, c2 := charsOf(s1), charsOf(s2)
		if n <= len(c1) {
			return s1.Slice(len(c1)-n+1, len(c1)+1), true
		}
		// Copies of s2 from the left end, then s1 at the right.
		buf := make([]string, n)
		for i := 0; i < n; {
			for j := 0; j < len(c2) && i < n; j++ {
				buf[i] = c2[j]
				i++
			}
		}
		copy(buf[n-len(c1):], c1)
		return join(buf, joinUnit(s1, s2)), true
	})
	fn("center", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1, c2 := charsOf(s1), charsOf(s2)
		if n <= len(c1) {
			i := (len(c1)-n+1)>>1 + 1
			return s1.Slice(i, i+n), true
		}
		// The right half is filled with copies of s2 from the right end,
		// the left half from the left end; then s1 goes in the middle.
		buf := make([]string, n)
		h := n / 2
		for i := n; i > h; {
			for j := len(c2); j > 0 && i > h; j-- {
				i--
				buf[i] = c2[j-1]
			}
		}
		for i := 0; i < h; {
			for j := 0; j < len(c2) && i < h; j++ {
				buf[i] = c2[j]
				i++
			}
		}
		odd := 0
		if n%2 == 0 && len(c1)%2 == 1 {
			odd = 1
		}
		copy(buf[h-len(c1)/2-odd:], c1)
		return join(buf, joinUnit(s1, s2)), true
	})
	fn("repl", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		n := MustInt(arg(a, 1))
		if n < 0 || (len(s.s) > 0 && n > Int(1<<31/len(s.s))) {
			Runerr(205, n)
		}
		return NewString(strings.Repeat(s.s, int(n)), s.unit), true
	})
	fn("reverse", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		cs := charsOf(s)
		for i, j := 0, len(cs)-1; i < j; i, j = i+1, j-1 {
			cs[i], cs[j] = cs[j], cs[i]
		}
		return join(cs, s.unit), true
	})
	fn("trim", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		c := csetArg(a, 1, csetFrom(" ", TextUnit))
		var offs []int // byte offset of each character
		for i := 0; i < len(s.s); i += s.width(i) {
			offs = append(offs, i)
		}
		n := len(offs)
		for n > 0 {
			if _, in := c.matchAt(s, offs[n-1]); !in {
				break
			}
			n--
		}
		return s.Slice(1, n+1), true
	})
	fn("map", func(vm *VM, a []Value) (Value, bool) {
		s1 := MustStr(arg(a, 0))
		s2, s3 := CsetUcase.String(), CsetLcase.String()
		if _, null := arg(a, 1).(Null); !null {
			s2 = MustStr(a[1])
		}
		if _, null := arg(a, 2).(Null); !null {
			s3 = MustStr(a[2])
		}
		c2, c3 := charsOf(s2), charsOf(s3)
		if len(c2) != len(c3) {
			Runerr(208)
		}
		m := make(map[string]string, len(c2))
		for i, c := range c2 {
			m[c] = c3[i] // later occurrences take precedence, as in iconx
		}
		cs := charsOf(s1)
		for i, c := range cs {
			if r, ok := m[c]; ok {
				cs[i] = r
			}
		}
		return join(cs, joinUnit(s1, s3)), true
	})
	fn("detab", func(vm *VM, a []Value) (Value, bool) { return tabs(a, false), true })
	fn("entab", func(vm *VM, a []Value) (Value, bool) { return tabs(a, true), true })
}

// tabStops is the tab-stop state of detab and entab (iconx's nxttab).
type tabStops struct {
	stops          []int
	next           int // index of the next stop in stops
	last, interval int
}

func newTabStops(stops []int) *tabStops {
	t := &tabStops{stops: stops, last: 1, interval: 8}
	return t
}

func (t *tabStops) reset() { t.next, t.last = 0, 1 }

// after returns the next tab stop beyond col.
func (t *tabStops) after(col int) int {
	for t.next < len(t.stops) && col >= t.stops[t.next] {
		t.next++
		if t.next == len(t.stops) {
			t.interval = t.stops[t.next-1] - t.last
		} else {
			t.last = t.stops[t.next-1]
		}
	}
	if t.next >= len(t.stops) {
		return col + t.interval - (col-t.last)%t.interval
	}
	return t.stops[t.next]
}

// printable reports whether a character takes a column: in iconx, a
// byte for which C's isprint is true; in text, also any printable code
// point beyond ASCII.
func printable(c string, u Unit) bool {
	if len(c) == 1 {
		return c[0] >= ' ' && c[0] < 0x7F
	}
	r, _ := utf8.DecodeRuneInString(c)
	return u == TextUnit && r != utf8.RuneError && unicode.IsPrint(r)
}

// tabs implements detab(s, i...) and, if entab, entab(s, i...).
func tabs(a []Value, entab bool) *String {
	s := MustStr(arg(a, 0))
	var stops []int
	for k, v := range a[min(1, len(a)):] {
		i := int(MustInt(v))
		if (k == 0 && i <= 1) || (k > 0 && i <= stops[k-1]) {
			Runerr(210, v)
		}
		stops = append(stops, i)
	}
	t := newTabStops(stops)
	cs := charsOf(s)
	var out []string
	col := 1
	for k := 0; k < len(cs); k++ {
		c := cs[k]
		switch {
		case c == "\b":
			out = append(out, c)
			col--
			t.reset()
		case c == "\n" || c == "\r":
			out = append(out, c)
			col = 1
			t.reset()
		case c == "\t" && !entab:
			target := t.after(col)
			for ; col < target; col++ {
				out = append(out, " ")
			}
		case c == "\t":
			out = append(out, c)
			col = t.after(col)
		case c == " " && entab:
			target := col + 1
			for k+1 < len(cs) && cs[k+1] == " " {
				target++
				k++
			}
			if target-col <= 1 {
				out = append(out, " ")
				col = target
				break
			}
			nt := t.after(col)
			if nt == col+1 {
				nt1 := t.after(nt)
				if nt1 > target {
					out = append(out, " ") // keep a space rather than a one-column tab
					col++
					nt = nt1
				}
			}
			for nt <= target {
				out = append(out, "\t")
				col = nt
				nt = t.after(nt)
			}
			for ; col < target; col++ {
				out = append(out, " ")
			}
			col = target
		default:
			out = append(out, c)
			if printable(c, s.unit) {
				col++
			}
		}
	}
	return join(out, s.unit)
}
