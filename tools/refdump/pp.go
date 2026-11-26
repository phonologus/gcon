package main

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"github.com/phonologus/gcon/internal/ipp"
	"github.com/phonologus/gcon/internal/run"
)

// The pp form: for each output line, "L offset file line text" (the
// map's entry and the line, escaped), then "E file line message" for
// each error. The file is preprocessed with gcon's predefined symbols
// and the include path in $PPPATH (directories separated by spaces).
func pp(name, src string) {
	r := ipp.Preprocess(name, src, ipp.Options{
		Predefined:  run.Predefined(),
		IncludePath: strings.Fields(os.Getenv("PPPATH")),
	})
	var out strings.Builder
	rest := r.Src
	for off := 0; rest != ""; {
		i := strings.IndexByte(rest, '\n') + 1
		if i == 0 {
			i = len(rest)
		}
		file, line := r.Map.Locate(off)
		fmt.Fprintf(&out, "L %d %s %d %s\n", off, esc(file), line, esc(rest[:i]))
		off += i
		rest = rest[i:]
	}
	for _, e := range r.Errors {
		fmt.Fprintf(&out, "E %s %d %s\n", esc(e.File), e.Line, esc(e.Msg))
	}
	os.Stdout.WriteString(out.String())
}

// predef writes the names of gcon's predefined symbols, which are all
// defined as "1", sorted.
func predef() {
	var names []string
	for k, v := range run.Predefined() {
		if v != "1" {
			panic("predefined " + k + " is not 1")
		}
		names = append(names, k)
	}
	sort.Strings(names)
	fmt.Println(strings.Join(names, " "))
}
