package run

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// TestRandomIteration checks that traversals of tables and sets start at
// random entries, as Go randomises map iteration (docs/design.md), while
// each traversal still produces every entry once, skips entries deleted
// meanwhile, and gives variables for !T.
func TestRandomIteration(t *testing.T) {
	src := `procedure main()
   local t, s, first, k, n, ok, x
   t := table(0)
   every t[1 to 8] := 0
   s := set()
   every insert(s, 1 to 8)
   first := set()
   ok := "complete"
   every 1 to 100 do {
      n := 0
      every k := key(t) do { /x := k; n +:= k }
      insert(first, x); x := &null
      if n ~= 36 then ok := "incomplete"
      n := 0
      every n +:= !s
      if n ~= 36 then ok := "incomplete"
      }
   write(if *first > 1 then "varies" else "fixed", " ", ok)
   # deleting during a traversal: deleted keys are not produced
   n := 0
   every k := key(t) do { n +:= 1; every delete(t, 1 to 8) }
   write(n, " ", *t)
   # !t gives variables
   t := table(); every t[1 to 3] := 0
   every !t := 5
   n := 0; every n +:= !t
   write(n)
end
`
	dir := t.TempDir()
	path := filepath.Join(dir, "iter.icn")
	if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errs bytes.Buffer
	File(path, Config{Stdout: &out, Stderr: &errs})
	if want := "varies complete\n1 0\n15\n"; out.String() != want {
		t.Errorf("got %q, want %q; stderr: %s", out.String(), want, errs.String())
	}
}
