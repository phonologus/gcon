package run

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestUnicode checks gcon's Unicode behaviour, which has no oracle: iconx
// treats UTF-8 as bytes. The expected results follow from the rules in
// docs/design.md ("Strings and Unicode", "Csets", "image()"); each case
// cites the rule it checks.
func TestUnicode(t *testing.T) {
	tests := []struct{ name, body, want string }{
		// Positions, subscripts and lengths are bytes; len counts characters.
		{"length", `write(*"héllo", " ", len("héllo"), " ", *"日本語", " ", len("日本語"), " ", len(bytes("é")), " ", len(""))`,
			"6 5 9 3 2 0"},
		{"subscript", `write(image("héllo"[2]), " ", "héllo"[2:4], " ", image("日本語"[-1]), " ", "日本語"[4+:3])`,
			`"\xc3" é "\x9e" 本`},
		// Joining never changes lengths, even when it completes a character.
		{"join lengths", `s := runes("\xc3") || runes("\xa9"); write(*s, " ", len(s), " ", s)`, "2 1 é"},
		// move steps over characters; tab takes byte positions.
		{"move and tab", `"é日x" ? { move(1); writes(&pos, " "); move(1); writes(&pos, " "); move(-1); writes(&pos, " "); write(image(tab(2)), " ", image(tab(0))) }`,
			`3 6 3 "\xa9" "\xa9日x"`},
		{"bang", `every writes(!"aé日", "|"); write()`, "a|é|日|"},
		// image follows %q for text; bytes escape 0x80-0xFF.
		{"image", `write(image("é"), " ", image(bytes("é")), " ", image("\u00a0"), " ", image(uchar(16r1F600)))`,
			`"é" "\xc3\xa9" "\u00a0" "😀"`},
		// Malformed UTF-8: each invalid byte is one character, read as U+FFFD,
		// and never rewritten.
		{"lone bytes", `s := runes("caf\xe9"); write(*s, " ", ord(s[4]), " ", image(s), " ", *(s || "é"))`,
			`4 65533 "caf\xe9" 6`},
		{"bytes ord", `write(ord(bytes("é")[1]), " ", ord(char(200)), " ", ord("é"))`, "195 200 233"},
		// Mixed units: non-ASCII text makes text; else non-ASCII bytes make bytes.
		{"concat units", `write(len("é" || char(200)), " ", *("abc" || char(200)), " ", type(char(200)))`, "2 4 string"},
		{"escapes", `write("\u00e9" == "é", " ", *"\U01f600", " ", image("\U10ffff"))`, `é 4 "\U10ffff"`},
		{"uchar", `write(image(uchar(16rD800)), " ", image(uchar(-1)))`, `"�" "�"`},
		{"utf8valid", `write(image(utf8valid("é")) | "no", " ", image(utf8valid(bytes("\xff"))) | "invalid")`,
			`"é" invalid`},
		// Scanning positions are bytes; the tests step over characters.
		{"scan", `"naïve café" ? { write(tab(upto(' ')), " ", &pos); move(1); write(tab(0), " ", &pos) }`,
			"naïve 7\ncafé 13"},
		{"scan match", `"日本語テキスト" ? { write(="日本", " ", &pos, " ", tab(many('語テ')), " ", &pos) }`,
			"日本 13 語テ 13"}, // &pos is dereferenced when write is called
		{"find text", `every writes(find("é", "éaéaé"), " "); write()`, "1 4 7 "},
		// Csets.
		{"cset literal", `write(*'αβγα', " ", image('γβα'), " ", upto('αβγ', "xβy"))`, "3 'αβγ' 2"},
		{"complement", `"é x" ? write(tab(many(~' ')))`, "é"},
		{"ranges", `write('a..e', " ", *'\u0370..\u03ff', " ", image('a..z'), " ", image('\...0'), " ", '0..2xyz')`,
			"abcde 144 &lcase './0' 012xyz"},
		{"range across surrogates", `write(*'\ud7ff..\ue000')`, "2"},
		{"big images", `write(image(~&ascii), " ", image(~'a'), " ", image(~'/'))`,
			`'\u0080..\U10ffff\x80..\xff' '\x00..` + "`" + `b..\U10ffff\x80..\xff' '\x00..\.0..\U10ffff\x80..\xff'`},
		{"sizes", `write(*&uset, " ", *~&ascii, " ", *~&lcase, " ", image(&uset))`, "1112192 1112064 1112166 &uset"},
		// Csets have one universe, so complement is over all of it: ~&cset
		// is every code point from 0x80 (in 9.5, '').
		{"complement of &cset", `write(image(~&cset), " ", image(~~&cset), " ", *~&cset)`,
			`'\u0080..\U10ffff' &cset 1111936`},
		// The byte 0xE9 and the code point U+00E9 are different members.
		{"byte vs code point", `write(*'\xe9é', " ", image('\xe9' ** 'é'), " ", image('\xe9é'))`, `2 '' 'é\xe9'`},
		// bytes(c) is the bytes matching c in a bytes subject.
		{"bytes of cset", `c := bytes('\xc3\xa9'); write(*c, " ", image(runes(c) === c), " ", image(bytes('é')), " ", *bytes('é'))`,
			`2 '\xa9\xc3' '\xa9\xc3' 2`},
		// Matching across units.
		{"bytes subject", `write(upto('é', bytes("aé")), " ", image(upto(bytes('\xc3'), "aé")) | "no")`, "2 no"},
		// String functions count characters of the string's unit.
		{"padding", `write("[", left("é", 3), "][", right("日本", 3, "·"), "][", center("ü", 5, "-"), "][", left(bytes("é"), 3), "]")`,
			"[é  ][·日本][--ü--][\xc3\xa9 ]"},
		{"reverse map trim", `write(reverse("aé日"), " ", map("café", "é", "e"), " ", map("abc", "abc", "αβγ"), " ", trim("xyé··", '·'), " ", repl("é", 3))`,
			"日éa cafe αβγ xyé ééé"},
		{"detab text", `write(image(detab("é\tx")), " ", image(entab("é       x")))`, `"é       x" "é\tx"`},
		// Reading: text never splits a sequence; mode "u" reads bytes.
		{"reads units", `f := open("u.txt", "w"); writes(f, "é日"); close(f); ` +
			`f := open("u.txt"); s := reads(f, 1); write(*s, " ", image(s), " ", image(reads(f, 5))); close(f); ` +
			`f := open("u.txt", "u"); s := reads(f, 1); write(*s, " ", image(s), " ", image(read(f))); close(f)`,
			"2 \"é\" \"日\"\n1 \"\\xc3\" \"\\xa9\\xe6\\x97\\xa5\""},
		// Table keys are compared as === compares: units matter only for
		// non-ASCII strings; csets by members.
		{"table keys", `t := table(); t["é"] := 1; t[bytes("é")] := 2; t["a"] := 3; t[bytes("a")] := 4; t['ba'] := 5; t['ab'] := 6; write(*t, " ", t["a"], " ", t['ab'])`,
			"4 4 6"},
		{"lone byte match", `s := runes("a\xffb"); write(upto('\xff', s), " ", image(upto('\uFFFD', s)) | "no")`,
			"2 no"},
	}
	dir := t.TempDir()
	t.Chdir(dir) // programs may create files
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(dir, strings.ReplaceAll(tt.name, " ", "_")+".icn")
			src := "procedure main()\n   local s, c, f, t\n   " + tt.body + "\nend\n"
			if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
				t.Fatal(err)
			}
			var out, errs bytes.Buffer
			File(path, Config{Stdout: &out, Stderr: &errs})
			if got := strings.TrimSuffix(out.String(), "\n"); got != tt.want {
				t.Errorf("%s\ngot  %s\nwant %s\nstderr: %s", tt.body, got, tt.want, errs.String())
			}
		})
	}
}

// TestCsetLiteralErrors checks the errors in cset range literals.
func TestCsetLiteralErrors(t *testing.T) {
	for _, lit := range []string{`'..a'`, `'a..'`, `'a...b'`, `'z..a'`, `'éa..\xff'`} {
		_, errs := Translate("t.icn", "procedure main()\n   write("+lit+")\nend\n")
		if len(errs) == 0 {
			t.Errorf("%s: no error", lit)
		}
	}
}

// TestCsetAlgebra checks that csets form a Boolean algebra over one
// universe: complement is an involution, De Morgan's laws hold, and
// identical csets have identical complements.
func TestCsetAlgebra(t *testing.T) {
	csets := []string{`''`, `&cset`, `&ascii`, `&uset`, `'abc'`, `'\xe9é'`, `~&ascii`,
		`&cset -- &ascii`, `'α..ω\x80..\x8f'`, `bytes('é')`, `~'\xff'`}
	for i, c := range csets {
		csets[i] = "(" + c + ")" // operands are substituted into expressions
	}
	var body strings.Builder
	for _, a := range csets {
		fmt.Fprintf(&body, "   (~~%s === %s) | write(\"involution fails: \", image(%s))\n", a, a, a)
		for _, b := range csets {
			fmt.Fprintf(&body, "   (~(%s ++ %s) === (~%s ** ~%s)) | write(\"De Morgan ++ fails: \", image(%s), image(%s))\n", a, b, a, b, a, b)
			fmt.Fprintf(&body, "   (~(%s ** %s) === (~%s ++ ~%s)) | write(\"De Morgan ** fails: \", image(%s), image(%s))\n", a, b, a, b, a, b)
			fmt.Fprintf(&body, "   (%s -- %s === %s ** ~%s) | write(\"difference fails: \", image(%s), image(%s))\n", a, b, a, b, a, b)
			fmt.Fprintf(&body, "   if %s === %s then (~%s === ~%s) | write(\"substitution fails\")\n", a, b, a, b)
		}
	}
	path := filepath.Join(t.TempDir(), "algebra.icn")
	src := "procedure main()\n" + body.String() + "   write(\"ok\")\nend\n"
	if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errs bytes.Buffer
	File(path, Config{Stdout: &out, Stderr: &errs})
	if out.String() != "ok\n" {
		t.Errorf("%s%s", out.String(), errs.String())
	}
}
