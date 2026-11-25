package vm

import (
	"io"
	"strings"
	"testing"
)

func testVM() *VM {
	return New(&Program{Main: -1}, strings.NewReader(""), io.Discard, io.Discard)
}

// strCases are calls of the string functions: those micro.icn times
// (icon/tests/bench), and the same with non-ASCII text and with bytes.
var strCases = []struct {
	name string
	fn   string
	args []Value
	want string
}{
	{"reverse", "reverse", []Value{V(Text("abcdefghijklmnopqrstuvwxyz"))}, "zyxwvutsrqponmlkjihgfedcba"},
	{"reverse/text", "reverse", []Value{V(Text("aé€𝄞b"))}, "b𝄞€éa"},
	{"reverse/bytes", "reverse", []Value{V(NewString("a\xc3\xa9", BytesUnit))}, "\xa9\xc3a"},
	{"left", "left", []Value{V(Text("a")), V(Int(10))}, "a         "},
	{"left/pad", "left", []Value{V(Text("abc")), V(Int(8)), V(Text("xy"))}, "abcyxyxy"},
	{"left/text", "left", []Value{V(Text("é")), V(Int(4)), V(Text("€·"))}, "é·€·"},
	{"left/cut", "left", []Value{V(Text("aéb")), V(Int(2))}, "aé"},
	{"right", "right", []Value{V(Text("a")), V(Int(10))}, "         a"},
	{"right/pad", "right", []Value{V(Text("abc")), V(Int(8)), V(Text("xy"))}, "xyxyxabc"},
	{"right/text", "right", []Value{V(Text("é")), V(Int(4)), V(Text("€·"))}, "€·€é"},
	{"right/cut", "right", []Value{V(Text("aéb")), V(Int(2))}, "éb"},
	{"center", "center", []Value{V(Text("a")), V(Int(10))}, "    a     "},
	{"center/pad", "center", []Value{V(Text("ab")), V(Int(9)), V(Text("xy"))}, "xyxabxyxy"},
	{"center/text", "center", []Value{V(Text("é")), V(Int(5)), V(Text("€·"))}, "€·é€·"},
	{"center/cut", "center", []Value{V(Text("aébcd")), V(Int(2))}, "bc"},
	{"trim", "trim", []Value{V(Text("a         "))}, "a"},
	{"trim/cset", "trim", []Value{V(Text("xaéyé")), V(NewCsetLiteral([]string{"é"}, nil, TextUnit))}, "xaéy"},
	{"trim/all", "trim", []Value{V(Text("   "))}, ""},
	{"map", "map", []Value{V(Text("quick brown fox")), V(Text("abcdefghijklmnopqrstuvwxyz")), V(Text("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))}, "QUICK BROWN FOX"},
	{"map/small", "map", []Value{V(Text("b")), V(Text("ab")), V(Text("ba"))}, "a"},
	{"map/text", "map", []Value{V(Text("aébé")), V(Text("é")), V(Text("€"))}, "a€b€"},
	{"map/later", "map", []Value{V(Text("aba")), V(Text("aa")), V(Text("xy"))}, "yby"},
	{"map/default", "map", []Value{V(Text("Hello"))}, "hello"},
	{"detab", "detab", []Value{V(Text("a\tb\tc"))}, "a       b       c"},
	{"entab", "entab", []Value{V(Text("a         "))}, "a\t  "},
}

func callStr(vm *VM, fn string, args []Value) string {
	a := append([]Value(nil), args...)
	v, ok := Functions[fn].Fn(vm, a)
	if !ok {
		return "<failed>"
	}
	return v.Any().(String).s
}

func TestStrFuncs(t *testing.T) {
	vm := testVM()
	for _, c := range strCases {
		if got := callStr(vm, c.fn, c.args); got != c.want {
			t.Errorf("%s: got %q, want %q", c.name, got, c.want)
		}
	}
}

func BenchmarkStrFuncs(b *testing.B) {
	vm := testVM()
	for _, c := range strCases {
		b.Run(c.name, func(b *testing.B) {
			for range b.N {
				callStr(vm, c.fn, c.args)
			}
		})
	}
}
