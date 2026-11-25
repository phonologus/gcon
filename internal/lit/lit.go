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

// String decodes a string or cset literal token, quotes included. It
// returns the literal's bytes and their unit.
func String(tok string) (string, Unit, error) {
	if len(tok) < 2 || (tok[0] != '"' && tok[0] != '\'') || tok[len(tok)-1] != tok[0] {
		return "", ASCII, fmt.Errorf("malformed literal %s", tok)
	}
	src := tok[1 : len(tok)-1]
	var b strings.Builder
	codePoint := false // a non-ASCII code point has been seen
	highByte := false  // a non-ASCII byte that is not part of a code point
	cont := -1         // output length at a '_' followed only by blanks, else -1
	for i := 0; i < len(src); {
		c := src[i]
		switch {
		case c == '\n':
			// A '_' that was the last non-blank before the newline joins
			// the next line, from its first non-blank, dropping the '_'.
			if cont >= 0 {
				s := b.String()[:cont]
				b.Reset()
				b.WriteString(s)
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
				b.WriteByte(byte(v))
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
				b.WriteByte(byte(v))
			case e == '^' && i < len(src):
				b.WriteByte(src[i] & 037)
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
					return "", ASCII, fmt.Errorf("\\%c escape needs %d hex digits", e, min)
				}
				if v > utf8.MaxRune || (0xD800 <= v && v <= 0xDFFF) {
					return "", ASCII, fmt.Errorf("invalid code point U+%X", v)
				}
				if v >= 0x80 {
					codePoint = true
				}
				b.WriteRune(rune(v))
			default:
				if v, ok := esc[e]; ok {
					b.WriteByte(v)
				} else {
					if e >= 0x80 {
						i-- // an escaped non-ASCII character: decode it below
						continue
					}
					b.WriteByte(e)
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
			b.WriteString(src[i : i+w])
			i += w
			cont = -1
			continue
		}
		if c == '_' {
			cont = b.Len()
		} else if !isSpace(c) {
			cont = -1
		}
		b.WriteByte(c)
		i++
	}
	u := ASCII
	switch {
	case codePoint:
		u = Text
	case highByte:
		u = Bytes
	}
	return b.String(), u, nil
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
