package run

import (
	"io"
	"strconv"
	"strings"
	"testing"

	"github.com/phonologus/gcon/internal/vm"
)

// vmBenchmarks are loops of single operations, as micro.icn times them,
// for measuring and profiling the VM:
//
//	go test ./internal/run -run XXX -bench VM -cpuprofile cpu.out
var vmBenchmarks = []struct{ name, body string }{
	{"empty", `&null`},
	{"size", `*"abcde"`},
	{"upto", `"abcde" ? upto('d')`},
	{"find", `"abcde" ? find("de")`},
	{"add", `x + 407`},
	{"call", `p()`},
	{"subscript", `L[3]`},
	{"left", `left("a", 10)`},
	{"map", `map("quick brown fox", &lcase, &ucase)`},
	{"move", `"abcde" ? move(1)`},
	{"tableinsdel", `insert(T, 5) & delete(T, 5)`},
	{"push", `pop(push(L, 0))`},
}

func BenchmarkVM(b *testing.B) {
	for _, bm := range vmBenchmarks {
		b.Run(bm.name, func(b *testing.B) {
			src := "procedure p()\nend\nprocedure main()\n   local x, L, T\n   x := 7\n   L := [1, 2, 3]\n   T := table()\n" +
				"   every 1 to " + strconv.Itoa(b.N) + " do " + bm.body + "\nend\n"
			prog, errs := TranslateLinked("bench.icn", src, []string{}, []string{})
			if len(errs) > 0 {
				b.Fatal(errs)
			}
			m := vm.New(prog, strings.NewReader(""), io.Discard, io.Discard)
			b.ResetTimer()
			if s := m.Run(nil); s != 0 {
				b.Fatalf("status %d", s)
			}
		})
	}
}
