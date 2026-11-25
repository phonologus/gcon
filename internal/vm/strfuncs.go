package vm

import (
	"strings"
	"unicode"
	"unicode/utf8"
	"unsafe"
)

// String functions, following iconx's fstr.r. They work in characters
// of their strings' units, so that left("é", 3) pads to three characters.

// chars gives the characters of a string by index, without allocating
// when each character is a byte (a bytes string, or ASCII text), and
// otherwise through their byte offsets.
type chars struct {
	s    string
	offs []int // each character's byte offset, then len(s); nil if each is a byte
}

func charsIn(s String) chars {
	if oneByte(s) {
		return chars{s: s.s}
	}
	offs := make([]int, 0, len(s.s)+1)
	for i := 0; i < len(s.s); i += s.width(i) {
		offs = append(offs, i)
	}
	return chars{s: s.s, offs: append(offs, len(s.s))}
}

// oneByte reports whether each character of s is one byte.
func oneByte(s String) bool { return s.unit == BytesUnit || s.IsASCII() }

func (c chars) n() int {
	if c.offs == nil {
		return len(c.s)
	}
	return len(c.offs) - 1
}

// at returns character i (from 0).
func (c chars) at(i int) string {
	if c.offs == nil {
		return c.s[i : i+1]
	}
	return c.s[c.offs[i]:c.offs[i+1]]
}

// slice returns characters i to j (from 0, j excluded).
func (c chars) slice(i, j int) string {
	if c.offs == nil {
		return c.s[i:j]
	}
	return c.s[c.offs[i]:c.offs[j]]
}

// byteString returns b as a string without copying it; b must not be
// changed afterwards.
func byteString(b []byte) string {
	if len(b) == 0 {
		return ""
	}
	return unsafe.String(unsafe.SliceData(b), len(b))
}

// joinUnit returns the unit of a string made of characters from the
// given strings, by the rule for concatenation.
func joinUnit(ss ...String) Unit {
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
func padArgs(a []Value) (String, int, String) {
	s1 := MustStr(arg(a, 0))
	n := 1
	if _, null := arg(a, 1).AsNull(); !null {
		n = int(MustInt(a[1]))
	}
	if n < 0 {
		Runerr(205, IntVal(Int(n)))
	}
	s2 := blank
	if _, null := arg(a, 2).AsNull(); !null {
		if s := MustStr(a[2]); s.s != "" {
			s2 = s
		}
	}
	return s1, n, s2
}

// blank is the default pad of left, right and center.
var blank = Text(" ")

// trimSpace is trim's default cset.
var trimSpace = csetFrom(" ", TextUnit)

// The default second and third arguments of map.
var (
	mapUcase = CsetUcase.String()
	mapLcase = CsetLcase.String()
)

// padder writes the characters of a pad string c, of m characters,
// into a result of n characters, as iconx's left, right and center do.
type padder struct {
	c chars
	m int
}

// fromLeft writes the pad character for position p counting copies of
// the pad from the left end of the result.
func (pd padder) fromLeft(b *strings.Builder, p int) { pd.write(b, p%pd.m) }

// write writes pad character i.
func (pd padder) write(b *strings.Builder, i int) {
	if pd.c.offs == nil {
		b.WriteByte(pd.c.s[i])
		return
	}
	b.WriteString(pd.c.at(i))
}

// fromRight writes the pad character for position p of n counting
// copies of the pad from the right end of the result.
func (pd padder) fromRight(b *strings.Builder, p, n int) {
	pd.write(b, pd.m-1-(n-1-p)%pd.m)
}

func newPadder(s2 String) padder {
	c := charsIn(s2)
	return padder{c, c.n()}
}

// grow sizes b for a result of s1 and n-l pad characters of s2.
func grow(b *strings.Builder, s1, s2 String, pad, m int) {
	b.Grow(len(s1.s) + pad*((len(s2.s)+m-1)/m))
}

func init() {
	fn("left", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1 := charsIn(s1)
		l := c1.n()
		if n <= l {
			return StrVal(NewString(c1.slice(0, n), s1.unit)), true
		}
		// s1, then copies of s2 ending at the right end.
		pd := newPadder(s2)
		var b strings.Builder
		grow(&b, s1, s2, n-l, pd.m)
		b.WriteString(s1.s)
		for p := l; p < n; p++ {
			pd.fromRight(&b, p, n)
		}
		return StrVal(NewString(b.String(), joinUnit(s1, s2))), true
	})
	fn("right", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1 := charsIn(s1)
		l := c1.n()
		if n <= l {
			return StrVal(NewString(c1.slice(l-n, l), s1.unit)), true
		}
		// Copies of s2 from the left end, then s1.
		pd := newPadder(s2)
		var b strings.Builder
		grow(&b, s1, s2, n-l, pd.m)
		for p := 0; p < n-l; p++ {
			pd.fromLeft(&b, p)
		}
		b.WriteString(s1.s)
		return StrVal(NewString(b.String(), joinUnit(s1, s2))), true
	})
	fn("center", func(vm *VM, a []Value) (Value, bool) {
		s1, n, s2 := padArgs(a)
		c1 := charsIn(s1)
		l := c1.n()
		if n <= l {
			i := (l - n + 1) >> 1
			return StrVal(NewString(c1.slice(i, i+n), s1.unit)), true
		}
		// The left half is padded with copies of s2 from the left end,
		// the right half with copies from the right end, and s1 goes in
		// the middle, a character left of centre if n is even and l odd.
		pd := newPadder(s2)
		var b strings.Builder
		grow(&b, s1, s2, n-l, pd.m)
		h := n / 2
		start := h - l/2
		if n%2 == 0 && l%2 == 1 {
			start--
		}
		for p := 0; p < n; p++ {
			switch {
			case p == start && l > 0:
				b.WriteString(s1.s)
				p += l - 1
			case p < h:
				pd.fromLeft(&b, p)
			default:
				pd.fromRight(&b, p, n)
			}
		}
		return StrVal(NewString(b.String(), joinUnit(s1, s2))), true
	})
	fn("repl", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		n := MustInt(arg(a, 1))
		if n < 0 || (len(s.s) > 0 && n > Int(1<<31/len(s.s))) {
			Runerr(205, IntVal(n))
		}
		return StrVal(NewString(strings.Repeat(s.s, int(n)), s.unit)), true
	})
	fn("reverse", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		b := make([]byte, len(s.s))
		if oneByte(s) {
			for i, j := 0, len(b)-1; j >= 0; i, j = i+1, j-1 {
				b[i] = s.s[j]
			}
		} else {
			// Each character's bytes, in order, from the end.
			for i, p := 0, len(b); i < len(s.s); {
				w := s.width(i)
				p -= w
				copy(b[p:], s.s[i:i+w])
				i += w
			}
		}
		return StrVal(NewString(byteString(b), s.unit)), true
	})
	fn("trim", func(vm *VM, a []Value) (Value, bool) {
		s := MustStr(arg(a, 0))
		c := csetArg(a, 1, trimSpace)
		end := len(s.s)
		if oneByte(s) {
			for end > 0 {
				if _, in := c.matchAt(s, end-1); !in {
					break
				}
				end--
			}
		} else {
			cs := charsIn(s)
			k := cs.n()
			for k > 0 {
				if _, in := c.matchAt(s, cs.offs[k-1]); !in {
					break
				}
				k--
			}
			end = cs.offs[k]
		}
		return StrVal(s.Slice(1, end+1)), true
	})
	fn("map", func(vm *VM, a []Value) (Value, bool) {
		s1 := MustStr(arg(a, 0))
		s2, s3 := mapUcase, mapLcase
		if _, null := arg(a, 1).AsNull(); !null {
			s2 = MustStr(a[1])
		}
		if _, null := arg(a, 2).AsNull(); !null {
			s3 = MustStr(a[2])
		}
		t := vm.mapTab
		if t == nil || t.s2 != s2.s || t.u2 != s2.unit || t.s3 != s3.s || t.u3 != s3.unit {
			t = newMapTable(s2, s3)
			vm.mapTab = t
		}
		return StrVal(NewString(t.apply(s1), joinUnit(s1, s3))), true
	})
	fn("detab", func(vm *VM, a []Value) (Value, bool) { return StrVal(tabs(a, false)), true })
	fn("entab", func(vm *VM, a []Value) (Value, bool) { return StrVal(tabs(a, true)), true })
}

// mapTable is map's translation from the characters of s2 to those of
// s3. A VM keeps the last one it made, as iconx does, since a program
// usually maps many strings with the same s2 and s3.
type mapTable struct {
	s2, s3 string
	u2, u3 Unit
	bytes  *[256]byte        // when every character of s2 and s3 is a byte
	m      map[string]string // otherwise
}

func newMapTable(s2, s3 String) *mapTable {
	c2, c3 := charsIn(s2), charsIn(s3)
	if c2.n() != c3.n() {
		Runerr(208)
	}
	t := &mapTable{s2: s2.s, s3: s3.s, u2: s2.unit, u3: s3.unit}
	// Later occurrences in s2 take precedence, as in iconx.
	if c2.offs == nil && c3.offs == nil {
		t.bytes = new([256]byte)
		for i := range t.bytes {
			t.bytes[i] = byte(i)
		}
		for i := 0; i < len(s2.s); i++ {
			t.bytes[s2.s[i]] = s3.s[i]
		}
		return t
	}
	t.m = make(map[string]string, c2.n())
	for i := range c2.n() {
		t.m[c2.at(i)] = c3.at(i)
	}
	return t
}

// apply returns the bytes of s mapped.
func (t *mapTable) apply(s String) string {
	if t.bytes != nil && oneByte(s) {
		b := make([]byte, len(s.s))
		for i := 0; i < len(s.s); i++ {
			b[i] = t.bytes[s.s[i]]
		}
		return byteString(b)
	}
	var b strings.Builder
	b.Grow(len(s.s))
	for i := 0; i < len(s.s); {
		w := s.width(i)
		c := s.s[i : i+w]
		switch {
		case t.bytes != nil && w == 1:
			b.WriteByte(t.bytes[c[0]])
		case t.bytes != nil:
			b.WriteString(c) // longer than any character of s2
		default:
			if r, ok := t.m[c]; ok {
				c = r
			}
			b.WriteString(c)
		}
		i += w
	}
	return b.String()
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
func tabs(a []Value, entab bool) String {
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
	var b strings.Builder
	b.Grow(len(s.s))
	col := 1
	for k := 0; k < len(s.s); {
		w := s.width(k)
		c := s.s[k : k+w]
		k += w
		switch {
		case c == "\b":
			b.WriteString(c)
			col--
			t.reset()
		case c == "\n" || c == "\r":
			b.WriteString(c)
			col = 1
			t.reset()
		case c == "\t" && !entab:
			target := t.after(col)
			for ; col < target; col++ {
				b.WriteByte(' ')
			}
		case c == "\t":
			b.WriteString(c)
			col = t.after(col)
		case c == " " && entab:
			target := col + 1
			for k < len(s.s) && s.s[k] == ' ' {
				target++
				k++
			}
			if target-col <= 1 {
				b.WriteByte(' ')
				col = target
				break
			}
			nt := t.after(col)
			if nt == col+1 {
				nt1 := t.after(nt)
				if nt1 > target {
					b.WriteByte(' ') // keep a space rather than a one-column tab
					col++
					nt = nt1
				}
			}
			for nt <= target {
				b.WriteByte('\t')
				col = nt
				nt = t.after(nt)
			}
			for ; col < target; col++ {
				b.WriteByte(' ')
			}
			col = target
		default:
			b.WriteString(c)
			if printable(c, s.unit) {
				col++
			}
		}
	}
	return NewString(b.String(), s.unit)
}
