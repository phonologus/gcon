package ipp_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phonologus/gcon/internal/corpus"
	"github.com/phonologus/gcon/internal/ipp"
	"github.com/phonologus/gcon/internal/oracle"
	"github.com/phonologus/gcon/internal/run"
)

// TestOracle compares the preprocessor's output with "icont -E" on every
// corpus file: the same lines, ignoring blank lines (directive and skipped
// lines, which icont drops and ipp keeps empty) and icont's #line
// markers. It needs $ICONTOK (see tools/icontok/build.sh).
func TestOracle(t *testing.T) {
	o := oracle.New(t)
	// gcon's predefined symbols, and the two of the reference icont's
	// features that gcon lacks (both need C), so that the comparison is
	// of the preprocessing, not of the feature sets.
	predef := run.Predefined()
	predef["_DYNAMIC_LOADING"], predef["_EXTERNAL_VALUES"] = "1", "1"
	var n, compared int
	for _, file := range corpus.IconFiles(t) {
		n++
		want, ok := o.Expand(file)
		if !ok {
			continue // icont cannot preprocess it either (a missing include, say)
		}
		b, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		// ipp runs where icont did, so that includes resolve alike.
		t.Chdir(filepath.Dir(file))
		r := ipp.Preprocess(filepath.Base(file), string(b),
			ipp.Options{Predefined: predef, IncludePath: o.IncludePath()})
		if len(r.Errors) > 0 {
			t.Errorf("%s: %v", file, r.Errors[0])
			continue
		}
		if got, exp := lines(r.Src, false), lines(string(want), true); got != exp {
			t.Errorf("%s: preprocessed text differs from icont -E", file)
		}
		compared++
	}
	t.Logf("%d of %d files compared", compared, n)
}

// lines returns s's non-blank lines, without #line markers if hash.
func lines(s string, hash bool) string {
	var b strings.Builder
	for l := range strings.SplitSeq(s, "\n") {
		if strings.TrimSpace(l) == "" || hash && strings.HasPrefix(l, "#line ") {
			continue
		}
		b.WriteString(l + "\n")
	}
	return b.String()
}
