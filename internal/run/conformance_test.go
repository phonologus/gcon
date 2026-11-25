package run

import (
	"bytes"
	"flag"
	"io"
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
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	conformanceList := filepath.Join(wd, "testdata", "conformance.txt")
	root := corpus.Require(t)
	// The tests run in a copy of their directory, as Test-icon runs them
	// in theirs: some read and write files there, and the reference tree
	// is never modified.
	dir := t.TempDir()
	if err := os.CopyFS(dir, os.DirFS(filepath.Join(root, "tests", "general"))); err != nil {
		t.Fatal(err)
	}
	t.Chdir(dir)
	stds, err := filepath.Glob(filepath.Join(dir, "*.std"))
	if err != nil || len(stds) == 0 {
		t.Skip("no reference tests found")
	}
	listed := map[string]bool{}
	if b, err := os.ReadFile(conformanceList); err == nil {
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
		var stdin io.Reader
		if b, err := os.ReadFile(filepath.Join(dir, name+".dat")); err == nil {
			stdin = bytes.NewReader(b)
		}
		var out bytes.Buffer
		File(name+".icn", Config{Stdin: stdin, Stdout: &out, Stderr: &out, Timeout: 5 * time.Second})
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
		if err := os.WriteFile(conformanceList, []byte(b.String()), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}
