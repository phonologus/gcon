package run

import (
	"bytes"
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
		// Text strings count runes; bytes strings count bytes.
		{"length", `write(*"héllo", " ", *bytes("héllo"), " ", *"日本語")`, "5 6 3"},
		{"subscript", `write("héllo"[2], " ", "日本語"[-1], " ", "héllo"[2:4], " ", image(bytes("é")[1]))`,
			`é 語 él "\xc3"`},
		{"bang", `every writes(!"aé日", "|"); write()`, "a|é|日|"},
		// image follows %q for text; bytes escape 0x80-0xFF.
		{"image", `write(image("é"), " ", image(bytes("é")), " ", image("\u00a0"), " ", image(uchar(16r1F600)))`,
			`"é" "\xc3\xa9" "\u00a0" "😀"`},
		// Malformed UTF-8: each invalid byte is one character, read as U+FFFD,
		// and never rewritten.
		{"lone bytes", `s := text("caf\xe9"); write(*s, " ", ord(s[4]), " ", image(s), " ", *(s || "é"))`,
			`4 65533 "caf\xe9" 5`},
		{"bytes ord", `write(ord(bytes("é")[1]), " ", ord(char(200)), " ", ord("é"))`, "195 200 233"},
		// Mixed units: non-ASCII text makes text; else non-ASCII bytes make bytes.
		{"concat units", `write(*("é" || char(200)), " ", *("abc" || char(200)), " ", type(char(200)))`, "2 4 string"},
		{"escapes", `write("\u00e9" == "é", " ", *"\U01f600", " ", image("\U10ffff"))`, `é 1 "\U10ffff"`},
		{"uchar", `write(image(uchar(16rD800)), " ", image(uchar(-1)))`, `"�" "�"`},
		{"utf8valid", `write(image(utf8valid("é")) | "no", " ", image(utf8valid(bytes("\xff"))) | "invalid")`,
			`"é" invalid`},
		// Scanning text counts characters.
		{"scan", `"naïve café" ? { write(tab(upto(' ')), " ", &pos); move(1); write(tab(0), " ", &pos) }`,
			"naïve 6\ncafé 11"},
		{"scan match", `"日本語テキスト" ? { write(="日本", " ", &pos, " ", tab(many('語テ')), " ", &pos) }`,
			"日本 5 語テ 5"}, // &pos is dereferenced when write is called
		{"find text", `every writes(find("é", "éaéaé"), " "); write()`, "1 3 5 "},
		// Csets.
		{"cset literal", `write(*'αβγα', " ", image('γβα'), " ", upto('αβγ', "xβy"))`, "3 'αβγ' 2"},
		{"complement", `"é x" ? write(tab(many(~' ')))`, "é"},
		{"ranges", `write('a..e', " ", *'\u0370..\u03ff', " ", image('a..z'), " ", image('\...0'), " ", '0..2xyz')`,
			"abcde 144 &lcase './0' 012xyz"},
		{"range across surrogates", `write(*'\ud7ff..\ue000')`, "2"},
		{"big images", `write(image(~&ascii), " ", image(~'a'), " ", image(~'/'))`,
			`'\u0080..\U10ffff\x80..\xff' '\x00..` + "`" + `b..\U10ffff\x80..\xff' '\x00..\.0..\U10ffff\x80..\xff'`},
		{"sizes", `write(*&uset, " ", *~&ascii, " ", *~&lcase, " ", image(&uset))`, "1112192 1112064 1112166 &uset"},
		// A neutral cset complements as text: ~&cset is '', whose
		// complement is &uset, not &cset as in 9.5.
		{"double complement", `write(image(~&cset), " ", image(~~&cset))`, "'' &uset"},
		// Relabelling: bytes -> text -> bytes is lossless for csets.
		{"relabel", `c := bytes('\xc3\xa9'); write(*c, " ", *text(c), " ", image(bytes(text(c)) === c), " ", *bytes('é'))`,
			"2 2 '\\xa9\\xc3' 2"},
		// Matching across units.
		{"bytes subject", `write(upto('é', bytes("aé")), " ", image(upto(bytes('\xc3'), "aé")) | "no")`, "2 no"},
		{"lone byte match", `s := text("a\xffb"); write(upto(text(bytes('\xff')), s), " ", image(upto('\uFFFD', s)) | "no")`,
			"2 no"},
	}
	dir := t.TempDir()
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(dir, strings.ReplaceAll(tt.name, " ", "_")+".icn")
			src := "procedure main()\n   local s, c\n   " + tt.body + "\nend\n"
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
