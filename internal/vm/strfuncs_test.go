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
	{"reverse", "reverse", []Value{Text("abcdefghijklmnopqrstuvwxyz")}, "zyxwvutsrqponmlkjihgfedcba"},
	{"reverse/text", "reverse", []Value{Text("aé€𝄞b")}, "b𝄞€éa"},
	{"reverse/bytes", "reverse", []Value{NewString("a\xc3\xa9", BytesUnit)}, "\xa9\xc3a"},
	{"left", "left", []Value{Text("a"), Int(10)}, "a         "},
	{"left/pad", "left", []Value{Text("abc"), Int(8), Text("xy")}, "abcyxyxy"},
	{"left/text", "left", []Value{Text("é"), Int(4), Text("€·")}, "é·€·"},
	{"left/cut", "left", []Value{Text("aéb"), Int(2)}, "aé"},
	{"right", "right", []Value{Text("a"), Int(10)}, "         a"},
	{"right/pad", "right", []Value{Text("abc"), Int(8), Text("xy")}, "xyxyxabc"},
	{"right/text", "right", []Value{Text("é"), Int(4), Text("€·")}, "€·€é"},
	{"right/cut", "right", []Value{Text("aéb"), Int(2)}, "éb"},
	{"center", "center", []Value{Text("a"), Int(10)}, "    a     "},
	{"center/pad", "center", []Value{Text("ab"), Int(9), Text("xy")}, "xyxabxyxy"},
	{"center/text", "center", []Value{Text("é"), Int(5), Text("€·")}, "€·é€·"},
	{"center/cut", "center", []Value{Text("aébcd"), Int(2)}, "bc"},
	{"trim", "trim", []Value{Text("a         ")}, "a"},
	{"trim/cset", "trim", []Value{Text("xaéyé"), NewCsetLiteral([]string{"é"}, nil, TextUnit)}, "xaéy"},
	{"trim/all", "trim", []Value{Text("   ")}, ""},
	{"map", "map", []Value{Text("quick brown fox"), Text("abcdefghijklmnopqrstuvwxyz"), Text("ABCDEFGHIJKLMNOPQRSTUVWXYZ")}, "QUICK BROWN FOX"},
	{"map/small", "map", []Value{Text("b"), Text("ab"), Text("ba")}, "a"},
	{"map/text", "map", []Value{Text("aébé"), Text("é"), Text("€")}, "a€b€"},
	{"map/later", "map", []Value{Text("aba"), Text("aa"), Text("xy")}, "yby"},
	{"map/default", "map", []Value{Text("Hello")}, "hello"},
	{"detab", "detab", []Value{Text("a\tb\tc")}, "a       b       c"},
	{"entab", "entab", []Value{Text("a         ")}, "a\t  "},
}

func callStr(vm *VM, fn string, args []Value) string {
	a := append([]Value(nil), args...)
	v, ok := Functions[fn].Fn(vm, a)
	if !ok {
		return "<failed>"
	}
	return v.(*String).s
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
