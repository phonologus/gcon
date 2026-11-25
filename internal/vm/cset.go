package vm

import (
	"iter"
	"math/bits"
	"sort"
	"strings"
	"unicode/utf8"
)

// Cset is an Icon cset (docs/design.md, "Csets"): a set of characters
// drawn from one universe: the ASCII characters, the 128 non-ASCII bytes,
// and the code points from 0x80 that UTF-8 can encode. A byte such as
// 0xE9 and a code point such as U+00E9 are different members. Csets have
// no unit, so they form one Boolean algebra: ~~c === c, De Morgan's laws
// hold, and === is equality of members.
//
// The ASCII characters and the non-ASCII bytes are held in bits; code
// points in ranges.
type Cset struct {
	bits   [4]uint64
	ranges []runeRange // sorted, disjoint, not adjacent; >= 0x80; no surrogates
	n      int         // member count, -1 if not yet known
	bytes  *[4]uint64  // cache: the bits of the cset read as bytes
	str    String      // cache: the cset as a string (String)
	strOK  bool        // str is set
	compl  *Cset       // cache: the complement (a cset does not change once made)
}

type runeRange struct{ lo, hi rune }

const (
	surrLo = 0xD800
	surrHi = 0xDFFF
)

func (c *Cset) has(b byte) bool { return c.bits[b>>6]&(1<<(b&63)) != 0 }
func (c *Cset) set(b byte)      { c.bits[b>>6] |= 1 << (b & 63) }

// Size returns the number of members.
func (c *Cset) Size() int {
	if c.n < 0 {
		n := 0
		for _, w := range c.bits {
			n += bits.OnesCount64(w)
		}
		for _, r := range c.ranges {
			n += int(r.hi - r.lo + 1)
		}
		c.n = n
	}
	return c.n
}

func (c *Cset) hasRune(r rune) bool {
	i := sort.Search(len(c.ranges), func(i int) bool { return c.ranges[i].hi >= r })
	return i < len(c.ranges) && c.ranges[i].lo <= r
}

// bytesBits returns the bytes that match c in a bytes subject: its byte
// members, and the bytes of its code points' UTF-8 encodings.
func (c *Cset) bytesBits() *[4]uint64 {
	if len(c.ranges) == 0 {
		return &c.bits
	}
	if c.bytes == nil {
		b := c.bits
		set := func(x byte) { b[x>>6] |= 1 << (x & 63) }
		var buf [4]byte
		for _, r := range c.ranges {
			if r.hi-r.lo+1 >= 64 {
				// 64 consecutive code points cover every final
				// continuation byte, and with them every continuation
				// byte; the lead bytes run from lo's to hi's.
				for x := 0x80; x <= 0xBF; x++ {
					set(byte(x))
				}
				utf8.EncodeRune(buf[:], r.lo)
				lo := buf[0]
				utf8.EncodeRune(buf[:], r.hi)
				for x := int(lo); x <= int(buf[0]); x++ {
					set(byte(x))
				}
				continue
			}
			for x := r.lo; x <= r.hi; x++ {
				n := utf8.EncodeRune(buf[:], x)
				for _, y := range buf[:n] {
					set(y)
				}
			}
		}
		c.bytes = &b
	}
	return c.bytes
}

// MatchByte reports whether byte b of a bytes subject is in c: a byte
// member, or a byte of a member code point's encoding.
func (c *Cset) MatchByte(b byte) bool {
	bb := c.bytesBits()
	return bb[b>>6]&(1<<(b&63)) != 0
}

// MatchText reports whether the character of a text subject at byte
// offset i of s is in c, and returns the character's width. An invalid
// byte of the subject is that byte.
func (c *Cset) MatchText(s string, i int) (int, bool) {
	if s[i] < utf8.RuneSelf {
		return 1, c.has(s[i])
	}
	r, w := utf8.DecodeRuneInString(s[i:])
	if r == utf8.RuneError && w == 1 {
		return 1, c.has(s[i])
	}
	return w, c.hasRune(r)
}

// AsBytes returns the cset of the bytes that match c in a bytes subject
// (bytes(c)): a code point is replaced by the bytes of its encoding.
func (c *Cset) AsBytes() *Cset {
	if len(c.ranges) == 0 {
		return c
	}
	return &Cset{bits: *c.bytesBits(), n: -1}
}

// CsetOp computes a union ('+'), intersection ('*') or difference ('-').
func CsetOp(op byte, a, b *Cset) *Cset {
	r := &Cset{n: -1}
	for i := range r.bits {
		switch op {
		case '+':
			r.bits[i] = a.bits[i] | b.bits[i]
		case '*':
			r.bits[i] = a.bits[i] & b.bits[i]
		case '-':
			r.bits[i] = a.bits[i] &^ b.bits[i]
		}
	}
	switch op {
	case '+':
		r.ranges = unionRanges(a.ranges, b.ranges)
	case '*':
		r.ranges = interRanges(a.ranges, b.ranges)
	case '-':
		r.ranges = interRanges(a.ranges, complementRanges(b.ranges))
	}
	return r
}

// Complement returns ~c: every character of the universe not in c.
func Complement(c *Cset) *Cset {
	if c.compl != nil {
		return c.compl
	}
	r := &Cset{n: -1, ranges: complementRanges(c.ranges)}
	for i := range r.bits {
		r.bits[i] = ^c.bits[i]
	}
	r.compl = c // the complement's complement
	c.compl = r
	return r
}

// universe is the code points from 0x80 that UTF-8 can encode.
var universe = []runeRange{{0x80, surrLo - 1}, {surrHi + 1, utf8.MaxRune}}

func complementRanges(rs []runeRange) []runeRange {
	return interRanges(universe, invert(rs))
}

// invert returns the complement of rs within 0x80..MaxRune.
func invert(rs []runeRange) []runeRange {
	var out []runeRange
	next := rune(0x80)
	for _, r := range rs {
		if r.lo > next {
			out = append(out, runeRange{next, r.lo - 1})
		}
		next = r.hi + 1
	}
	if next <= utf8.MaxRune {
		out = append(out, runeRange{next, utf8.MaxRune})
	}
	return out
}

func unionRanges(a, b []runeRange) []runeRange {
	all := append(append([]runeRange(nil), a...), b...)
	sort.Slice(all, func(i, j int) bool { return all[i].lo < all[j].lo })
	return normalize(all)
}

// normalize merges sorted ranges that overlap or touch.
func normalize(rs []runeRange) []runeRange {
	var out []runeRange
	for _, r := range rs {
		if n := len(out); n > 0 && r.lo <= out[n-1].hi+1 {
			if r.hi > out[n-1].hi {
				out[n-1].hi = r.hi
			}
			continue
		}
		out = append(out, r)
	}
	return out
}

func interRanges(a, b []runeRange) []runeRange {
	var out []runeRange
	for i, j := 0, 0; i < len(a) && j < len(b); {
		lo, hi := max(a[i].lo, b[j].lo), min(a[i].hi, b[j].hi)
		if lo <= hi {
			out = append(out, runeRange{lo, hi})
		}
		if a[i].hi < b[j].hi {
			i++
		} else {
			j++
		}
	}
	return out
}

// addRunes adds code points (>= 0x80, valid) to c's ranges.
func (c *Cset) addRunes(rs []runeRange) {
	c.ranges = unionRanges(c.ranges, interRanges(universe, normalizeUnsorted(rs)))
	c.n, c.bytes = -1, nil
}

func normalizeUnsorted(rs []runeRange) []runeRange {
	rs = append([]runeRange(nil), rs...)
	sort.Slice(rs, func(i, j int) bool { return rs[i].lo < rs[j].lo })
	return normalize(rs)
}

// CsetOf returns the cset of the characters of s, read in s's unit.
func CsetOf(s String) *Cset {
	return csetFrom(s.s, s.unit)
}

func csetFrom(s string, u Unit) *Cset {
	c := &Cset{n: -1}
	if u == BytesUnit {
		for i := 0; i < len(s); i++ {
			c.set(s[i])
		}
		return c
	}
	var rs []runeRange
	for i := 0; i < len(s); {
		if s[i] < utf8.RuneSelf {
			c.set(s[i])
			i++
			continue
		}
		r, w := utf8.DecodeRuneInString(s[i:])
		if r == utf8.RuneError && w == 1 {
			c.set(s[i])
		} else {
			rs = append(rs, runeRange{r, r})
		}
		i += w
	}
	if len(rs) > 0 {
		c.addRunes(rs)
	}
	return c
}

// CsetRange is an inclusive range in a cset literal, as decoded by lit.
type CsetRange struct {
	Lo, Hi    rune
	LoneBytes bool // the ends are lone bytes (text) or bytes (bytes), not code points
}

// NewCsetLiteral builds the cset of a literal: the characters of its
// segments (each read separately, so that bytes never fuse across a
// range) and its ranges.
func NewCsetLiteral(segments []string, ranges []CsetRange, u Unit) *Cset {
	c := &Cset{n: -1}
	for _, s := range segments {
		c = CsetOp('+', c, csetFrom(s, u))
	}
	var rs []runeRange
	for _, r := range ranges {
		if r.LoneBytes || r.Hi < utf8.RuneSelf {
			for x := r.Lo; x <= r.Hi && x <= 0xFF; x++ {
				c.set(byte(x))
			}
			c.n = -1
			continue
		}
		if r.Lo < utf8.RuneSelf {
			for x := r.Lo; x < utf8.RuneSelf; x++ {
				c.set(byte(x))
			}
			rs = append(rs, runeRange{utf8.RuneSelf, r.Hi})
		} else {
			rs = append(rs, runeRange{r.Lo, r.Hi})
		}
	}
	if len(rs) > 0 {
		c.addRunes(rs)
	}
	return c
}

// Members generates the members of c in canonical (string) order, each
// as a one-character string.
func (c *Cset) Members() iter.Seq[string] {
	return func(yield func(string) bool) {
		if len(c.ranges) == 0 {
			for b := 0; b < 256; b++ {
				if c.has(byte(b)) && !yield(string([]byte{byte(b)})) {
					return
				}
			}
			return
		}
		for b := 0; b < utf8.RuneSelf; b++ {
			if c.has(byte(b)) && !yield(string([]byte{byte(b)})) {
				return
			}
		}
		// Lone bytes and code points, merged in byte-string order: a
		// lone byte precedes the code points whose encodings it starts.
		next := 0x80 // next lone byte to consider
		var buf [4]byte
		for _, r := range c.ranges {
			for x := r.lo; x <= r.hi; x++ {
				n := utf8.EncodeRune(buf[:], x)
				for ; next <= int(buf[0]); next++ {
					if c.has(byte(next)) && !yield(string([]byte{byte(next)})) {
						return
					}
				}
				if !yield(string(buf[:n])) {
					return
				}
			}
		}
		for ; next < 256; next++ {
			if c.has(byte(next)) && !yield(string([]byte{byte(next)})) {
				return
			}
		}
	}
}

// String returns c's canonical string: its members in order. It is
// text if c has a code point (its byte members are then lone bytes), else
// bytes: the unit a literal with those members would have.
func (c *Cset) String() String {
	if c.strOK {
		return c.str // a cset does not change once made
	}
	var b strings.Builder
	for m := range c.Members() {
		b.WriteString(m)
	}
	u := BytesUnit
	if len(c.ranges) > 0 {
		u = TextUnit
	}
	c.str, c.strOK = NewString(b.String(), u), true
	return c.str
}

// equalCsets reports whether a and b have the same members.
func equalCsets(a, b *Cset) bool {
	if a.bits != b.bits || len(a.ranges) != len(b.ranges) {
		return false
	}
	for i := range a.ranges {
		if a.ranges[i] != b.ranges[i] {
			return false
		}
	}
	return true
}

// Predefined csets.
var (
	CsetAll     = &Cset{bits: [4]uint64{^uint64(0), ^uint64(0), ^uint64(0), ^uint64(0)}, n: -1}
	CsetASCII   = &Cset{bits: [4]uint64{^uint64(0), ^uint64(0)}, n: -1}
	CsetDigits  = csetFrom("0123456789", TextUnit)
	CsetLcase   = csetFrom("abcdefghijklmnopqrstuvwxyz", TextUnit)
	CsetUcase   = csetFrom("ABCDEFGHIJKLMNOPQRSTUVWXYZ", TextUnit)
	CsetLetters = CsetOp('+', CsetLcase, CsetUcase)
	CsetUniv    = Complement(&Cset{n: -1})
)

// csetName returns the keyword naming a cset with c's members, as
// iconx's csname does (src/runtime/rmisc.r), or "".
func csetName(c *Cset) string {
	for _, k := range []struct {
		name string
		c    *Cset
	}{
		{"&letters", CsetLetters}, {"&lcase", CsetLcase}, {"&ucase", CsetUcase},
		{"&digits", CsetDigits}, {"&ascii", CsetASCII}, {"&cset", CsetAll}, {"&uset", CsetUniv},
	} {
		if equalCsets(c, k.c) {
			return k.name
		}
	}
	return ""
}

// imageCset returns image(c): a predefined cset's name; else, for up to
// 256 members, every member listed, as iconx does; else ranges (see
// docs/design.md, "image()").
func imageCset(c *Cset) string {
	if n := csetName(c); n != "" {
		return n
	}
	if c.Size() <= 256 {
		return quote(c.String(), '\'')
	}
	var b strings.Builder
	b.WriteByte('\'')
	// Code points, with ASCII members joined to the ranges above them and
	// runs across the surrogates joined (a literal range skips them).
	var cps []runeRange
	for x := 0; x < utf8.RuneSelf; x++ {
		if c.has(byte(x)) {
			cps = append(cps, runeRange{rune(x), rune(x)})
		}
	}
	cps = normalize(append(cps, c.ranges...))
	var runs []runeRange
	for _, r := range cps {
		if n := len(runs); n > 0 && runs[n-1].hi == surrLo-1 && r.lo == surrHi+1 {
			runs[n-1].hi = r.hi
			continue
		}
		runs = append(runs, r)
	}
	writeRuns(&b, runs, func(r rune) string { return imageChar(r, false) })
	var lone []runeRange
	for x := 0x80; x < 256; x++ {
		if c.has(byte(x)) {
			lone = append(lone, runeRange{rune(x), rune(x)})
		}
	}
	writeRuns(&b, normalize(lone), func(r rune) string { return imageChar(r, true) })
	b.WriteByte('\'')
	return b.String()
}

// writeRuns writes runs of 4 or more as x..y, shorter runs listed, with a
// '.' at either end of a range escaped.
func writeRuns(b *strings.Builder, runs []runeRange, ch func(rune) string) {
	end := func(r rune) string {
		if r == '.' {
			return `\.`
		}
		return ch(r)
	}
	for _, r := range runs {
		size := r.hi - r.lo + 1
		if r.lo < surrLo && r.hi > surrHi {
			size -= surrHi - surrLo + 1
		}
		if size >= 4 {
			b.WriteString(end(r.lo))
			b.WriteString("..")
			b.WriteString(end(r.hi))
			continue
		}
		for x := r.lo; x <= r.hi; x++ {
			if x >= surrLo && x <= surrHi {
				continue
			}
			b.WriteString(ch(x))
		}
	}
}

// imageChar renders one character of a cset image.
func imageChar(r rune, lone bool) string {
	var b strings.Builder
	if lone {
		quoteByte(&b, byte(r), '\'')
		return b.String()
	}
	s := NewString(string(r), TextUnit)
	q := quote(s, '\'')
	return q[1 : len(q)-1]
}
