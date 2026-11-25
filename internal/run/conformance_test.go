package run

import (
	"bytes"
	"flag"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/phonologus/gcon/internal/corpus"
)

var update = flag.Bool("update", false, "rewrite testdata/conformance.txt with the tests that pass")

// TestConformance runs the reference implementation's test programs
// (icon/tests/general) as its Test-icon script does, comparing standard
// output and standard error together with FILE.std.
//
// testdata/conformance.txt lists the tests gcon passes. A listed test
// that fails is a regression; a passing test that is not listed must be
// added (go test -run Conformance -update rewrites the list), so the list
// records progress exactly.
func TestConformance(t *testing.T) {
	root := corpus.Require(t)
	dir := filepath.Join(root, "tests", "general")
	stds, err := filepath.Glob(filepath.Join(dir, "*.std"))
	if err != nil || len(stds) == 0 {
		t.Skip("no reference tests found")
	}
	listed := map[string]bool{}
	if b, err := os.ReadFile("testdata/conformance.txt"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if l = strings.TrimSpace(l); l != "" && !strings.HasPrefix(l, "#") {
				listed[l] = true
			}
		}
	}
	var passed []string
	for _, std := range stds {
		name := strings.TrimSuffix(filepath.Base(std), ".std")
		src := filepath.Join(dir, name+".icn")
		if _, err := os.Stat(src); err != nil {
			continue
		}
		want, err := os.ReadFile(std)
		if err != nil {
			t.Fatal(err)
		}
		// TODO: Test-icon runs programs in their own directory, with
		// standard input from FILE.dat; gcon has no input or files yet.
		var out bytes.Buffer
		File(src, Config{Stdout: &out, Stderr: &out, Timeout: 5 * time.Second})
		ok := out.String() == string(want)
		switch {
		case ok:
			passed = append(passed, name)
			if !listed[name] && !*update {
				t.Errorf("%s passes but is not listed in testdata/conformance.txt", name)
			}
		case listed[name]:
			t.Errorf("%s is listed in testdata/conformance.txt but fails", name)
		}
	}
	sort.Strings(passed)
	t.Logf("%d of %d reference tests pass", len(passed), len(stds))
	if *update {
		var b strings.Builder
		b.WriteString("# Reference tests (icon/tests/general) that gcon passes.\n")
		b.WriteString("# Maintained by: go test ./internal/run -run Conformance -update\n")
		for _, n := range passed {
			b.WriteString(n + "\n")
		}
		if err := os.WriteFile("testdata/conformance.txt", []byte(b.String()), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}
