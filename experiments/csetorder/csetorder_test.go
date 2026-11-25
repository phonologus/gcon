// Package csetorder checks the property behind gcon's cset model
// (docs/design.md, "Csets"): the members of a text cset, sorted as byte
// strings and concatenated, decode back into exactly those members; and
// the sorted bytes of a bytes cset, read as text, are all lone bytes.
// It becomes a fuzz test of the real cset type once that exists.
package csetorder

import (
	"math/rand"
	"sort"
	"strings"
	"testing"
	"unicode/utf8"
)

// split reads s as text, as utf8.DecodeRuneInString does.
func split(s string) []string {
	var out []string
	for s != "" {
		_, w := utf8.DecodeRuneInString(s)
		out = append(out, s[:w])
		s = s[w:]
	}
	return out
}

func sorted(set map[string]bool) []string {
	var m []string
	for s := range set {
		m = append(m, s)
	}
	sort.Strings(m)
	return m
}

func TestTextCsetReadsBack(t *testing.T) {
	var pool []string
	for b := 0; b < 256; b++ {
		if s := string([]byte{byte(b)}); b < 0x80 || !utf8.ValidString(s) {
			pool = append(pool, s) // ASCII, or a lone invalid byte
		}
	}
	for _, r := range []rune{0x80, 0xBF, 0xC0, 0x7FF, 0x800, 0xFFF, 0x1000,
		0xD7FF, 0xE000, 0xFFFD, 0xFFFF, 0x10000, 0x3FFFF, 0x40000, 0x10FFFF} {
		pool = append(pool, string(r))
	}
	rng := rand.New(rand.NewSource(1))
	for i := 0; i < 2000; i++ {
		pool = append(pool, string(rune(0x80+rng.Intn(0x10FFFF-0x80))))
	}
	for trial := 0; trial < 50000; trial++ {
		set := map[string]bool{}
		for k := rng.Intn(12); k >= 0; k-- {
			set[pool[rng.Intn(len(pool))]] = true
		}
		m := sorted(set)
		if got := split(strings.Join(m, "")); strings.Join(got, "|") != strings.Join(m, "|") {
			t.Fatalf("members %q read back as %q", m, got)
		}
	}
}

func TestBytesCsetReadsAsLoneBytes(t *testing.T) {
	rng := rand.New(rand.NewSource(2))
	for trial := 0; trial < 50000; trial++ {
		set := map[string]bool{}
		for k := rng.Intn(12); k >= 0; k-- {
			set[string([]byte{byte(rng.Intn(256))})] = true
		}
		m := sorted(set)
		if got := split(strings.Join(m, "")); len(got) != len(m) {
			t.Fatalf("bytes %q read as text gave %q", m, got)
		}
	}
}
