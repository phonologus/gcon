// Package lit decodes Icon literal tokens: strings, csets and numbers.
//
// String and cset escapes follow icont's getstring (src/common/yylex.h)
// and its escape table (src/h/esctab.h), plus gcon's \u and \U escapes;
// the unit of the result follows docs/design.md ("Strings and Unicode").
package lit

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"
)

// Unit is the character unit of a decoded string or cset literal.
type Unit int

const (
	ASCII Unit = iota // all ASCII: the same in either unit
	Text              // contains a non-ASCII code point
	Bytes             // contains a non-ASCII byte but no non-ASCII code point
)

func (u Unit) String() string { return [...]string{"ascii", "text", "bytes"}[u] }

// esc maps the character after a backslash to its value, for the
// escapes that are a single character (icont's esctab). Characters not
// listed stand for themselves. Note that \8 and \9 are backspace and tab:
// esctab's entries for them follow its octal-digit entries.
var esc = map[byte]byte{
	'b': '\b', 'B': '\b', 'd': 0177, 'D': 0177, 'e': 033, 'E': 033,
	'f': '\f', 'F': '\f', 'l': '\n', 'L': '\n', 'n': '\n', 'N': '\n',
	'r': '\r', 'R': '\r', 't': '\t', 'T': '\t', 'v': '\v', 'V': '\v',
	'8': 010, '9': 011,
}

func isOctal(c byte) bool { return '0' <= c && c <= '7' }

func hexVal(c byte) (int, bool) {
	switch {
	case '0' <= c && c <= '9':
		return int(c - '0'), true
	case 'a' <= c && c <= 'f':
		return int(c-'a') + 10, true
	case 'A' <= c && c <= 'F':
		return int(c-'A') + 10, true
	}
	return 0, false
}

func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

// piece is one decoded character of a literal: the bytes it stands for,
// and whether it was written as an escape.
type piece struct {
	s   string
	esc bool
}

// decode decodes a string or cset literal token, quotes included, into
// pieces, and returns the literal's unit.
func decode(tok string) ([]piece, Unit, error) {
	if len(tok) < 2 || (tok[0] != '"' && tok[0] != '\'') || tok[len(tok)-1] != tok[0] {
		return nil, ASCII, fmt.Errorf("malformed literal %s", tok)
	}
	src := tok[1 : len(tok)-1]
	var ps []piece
	codePoint := false // a non-ASCII code point has been seen
	highByte := false  // a non-ASCII byte that is not part of a code point
	cont := -1         // len(ps) at a '_' followed only by blanks, else -1
	add := func(s string, esc bool) { ps = append(ps, piece{s, esc}) }
	for i := 0; i < len(src); {
		c := src[i]
		switch {
		case c == '\n':
			// A '_' that was the last non-blank before the newline joins
			// the next line, from its first non-blank, dropping the '_'.
			if cont >= 0 {
				ps = ps[:cont]
			}
			i++
			for i < len(src) && isSpace(src[i]) {
				i++
			}
			cont = -1
			continue
		case c == '\\' && i+1 < len(src):
			cont = -1
			i++
			e := src[i]
			i++
			switch {
			case isOctal(e):
				v := int(e - '0')
				for n := 1; n < 3 && i < len(src) && isOctal(src[i]); n++ {
					v = v<<3 | int(src[i]-'0')
					i++
				}
				v &= 0377
				if v >= 0x80 {
					highByte = true
				}
				add(string([]byte{byte(v)}), true)
			case e == 'x' || e == 'X':
				// Up to two hex digits; none gives NUL, as in icont.
				v := 0
				for n := 0; n < 2 && i < len(src); n++ {
					d, ok := hexVal(src[i])
					if !ok {
						break
					}
					v = v<<4 | d
					i++
				}
				if v >= 0x80 {
					highByte = true
				}
				add(string([]byte{byte(v)}), true)
			case e == '^' && i < len(src):
				add(string([]byte{src[i] & 037}), true)
				i++
			case e == 'u' || e == 'U':
				min, max := 4, 4
				if e == 'U' {
					min, max = 1, 6
				}
				v, n := 0, 0
				for n < max && i < len(src) {
					d, ok := hexVal(src[i])
					if !ok {
						break
					}
					v = v<<4 | d
					i++
					n++
				}
				if n < min {
					return nil, ASCII, fmt.Errorf("\\%c escape needs %d hex digits", e, min)
				}
				if v > utf8.MaxRune || (0xD800 <= v && v <= 0xDFFF) {
					return nil, ASCII, fmt.Errorf("invalid code point U+%X", v)
				}
				if v >= 0x80 {
					codePoint = true
				}
				add(string(rune(v)), true)
			default:
				if v, ok := esc[e]; ok {
					add(string([]byte{v}), true)
				} else if e >= 0x80 {
					i-- // an escaped non-ASCII character: decode it below
					r, w := utf8.DecodeRuneInString(src[i:])
					if r == utf8.RuneError && w == 1 {
						highByte = true
					} else {
						codePoint = true
					}
					add(src[i:i+w], true)
					i += w
				} else {
					add(string([]byte{e}), true)
				}
			}
			continue
		case c >= 0x80:
			r, w := utf8.DecodeRuneInString(src[i:])
			if r == utf8.RuneError && w == 1 {
				highByte = true
			} else {
				codePoint = true
			}
			add(src[i:i+w], false)
			i += w
			cont = -1
			continue
		}
		if c == '_' {
			cont = len(ps)
		} else if !isSpace(c) {
			cont = -1
		}
		add(string([]byte{c}), false)
		i++
	}
	u := ASCII
	switch {
	case codePoint:
		u = Text
	case highByte:
		u = Bytes
	}
	return ps, u, nil
}

// String decodes a string or cset literal token, quotes included. It
// returns the literal's bytes and their unit.
func String(tok string) (string, Unit, error) {
	ps, u, err := decode(tok)
	if err != nil {
		return "", ASCII, err
	}
	var b strings.Builder
	for _, p := range ps {
		b.WriteString(p.s)
	}
	return b.String(), u, nil
}

// Range is an inclusive range in a cset literal. In a text literal its
// ends are code points, or both lone bytes (LoneBytes); in a bytes literal
// they are bytes (LoneBytes is then set too).
type Range struct {
	Lo, Hi    rune
	LoneBytes bool
}

// Cset decodes a cset literal token. An unescaped ".." between two
// characters is a range (docs/design.md, "Csets"). It returns the text of
// the literal between ranges, as separate segments (to be read
// separately, so that bytes cannot fuse across a range), the ranges, and
// the unit.
func Cset(tok string) ([]string, []Range, Unit, error) {
	ps, u, err := decode(tok)
	if err != nil {
		return nil, nil, ASCII, err
	}
	dot := func(i int) bool { return i < len(ps) && ps[i].s == "." && !ps[i].esc }
	var segs []string
	var b strings.Builder
	for i := 0; i < len(ps); i++ {
		if dot(i) && dot(i+1) {
			if dot(i + 2) {
				return nil, nil, ASCII, fmt.Errorf("three dots in cset literal %s", tok)
			}
			segs = append(segs, b.String())
			b.Reset()
			i++
			continue
		}
		b.WriteString(ps[i].s)
	}
	segs = append(segs, b.String())
	var ranges []Range
	for k := 0; k+1 < len(segs); k++ {
		lo, loLone, ok1 := lastChar(segs[k], u)
		hi, hiLone, ok2 := firstChar(segs[k+1], u)
		if !ok1 || !ok2 {
			return nil, nil, ASCII, fmt.Errorf("\"..\" needs a character on each side in %s", tok)
		}
		if u != Bytes && loLone != hiLone {
			return nil, nil, ASCII, fmt.Errorf("range mixes a code point and a byte in %s", tok)
		}
		if lo > hi {
			return nil, nil, ASCII, fmt.Errorf("descending range in %s", tok)
		}
		ranges = append(ranges, Range{Lo: lo, Hi: hi, LoneBytes: u == Bytes || loLone})
	}
	return segs, ranges, u, nil
}

// firstChar and lastChar return the first or last character of s read in
// unit u, and whether it is a byte (in bytes) or lone byte (in text).
func firstChar(s string, u Unit) (rune, bool, bool) {
	if s == "" {
		return 0, false, false
	}
	if u == Bytes || s[0] < utf8.RuneSelf {
		return rune(s[0]), u == Bytes && s[0] >= utf8.RuneSelf, true
	}
	r, w := utf8.DecodeRuneInString(s)
	if r == utf8.RuneError && w == 1 {
		return rune(s[0]), true, true
	}
	return r, false, true
}

func lastChar(s string, u Unit) (rune, bool, bool) {
	if s == "" {
		return 0, false, false
	}
	n := len(s) - 1
	if u == Bytes || s[n] < utf8.RuneSelf {
		return rune(s[n]), u == Bytes && s[n] >= utf8.RuneSelf, true
	}
	r, w := utf8.DecodeLastRuneInString(s)
	if r == utf8.RuneError && w == 1 {
		return rune(s[n]), true, true
	}
	return r, false, true
}

// Number is a decoded numeric literal.
type Number struct {
	Int    int64
	Real   float64
	IsReal bool
}

// ErrRange reports an integer literal too large for gcon's integers.
var ErrRange = fmt.Errorf("integer literal out of range")

// ParseNumber decodes an integer literal (decimal, or radix NrDIGITS) or
// a real literal.
func ParseNumber(tok string) (Number, error) {
	if i := strings.IndexAny(tok, "rR"); i > 0 {
		base, err := strconv.Atoi(tok[:i])
		if err != nil || base < 2 || base > 36 {
			return Number{}, fmt.Errorf("invalid radix in %s", tok)
		}
		v, err := strconv.ParseInt(tok[i+1:], base, 64)
		if err != nil {
			if ne, ok := err.(*strconv.NumError); ok && ne.Err == strconv.ErrRange {
				return Number{}, ErrRange
			}
			return Number{}, fmt.Errorf("invalid digits in %s", tok)
		}
		return Number{Int: v}, nil
	}
	if strings.ContainsAny(tok, ".eE") {
		f, err := strconv.ParseFloat(tok, 64)
		if err != nil {
			return Number{}, fmt.Errorf("invalid real literal %s", tok)
		}
		return Number{Real: f, IsReal: true}, nil
	}
	v, err := strconv.ParseInt(tok, 10, 64)
	if err != nil {
		if ne, ok := err.(*strconv.NumError); ok && ne.Err == strconv.ErrRange {
			return Number{}, ErrRange
		}
		return Number{}, fmt.Errorf("invalid integer literal %s", tok)
	}
	return Number{Int: v}, nil
}
