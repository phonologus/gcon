package run

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// repoRoot returns the repository, which is a gcon root (lib/ipl). It
// resolves the path from the package directory, so call it before any
// t.Chdir.
func repoRoot(t *testing.T) string {
	t.Helper()
	d, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return d
}

// TestRootLibrary checks the search order: link finds the IPL in the
// root, after the current directory and IPATH, and $include finds its
// include files after LPATH; a copy on IPATH shadows the root's.
func TestRootLibrary(t *testing.T) {
	root := repoRoot(t)
	t.Chdir(t.TempDir())
	src := "$include \"invkdefs.icn\"\nlink strings\nprocedure main()\n   write(replace(\"a-b\", \"-\", \"+\"))\nend\n"
	if err := os.WriteFile("p.icn", []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	run := func(cfg Config) (string, string) {
		var out, errs bytes.Buffer
		cfg.Stdout, cfg.Stderr = &out, &errs
		File("p.icn", cfg)
		return out.String(), errs.String()
	}
	if out, errs := run(Config{LibPath: []string{}, IncludePath: []string{}, Root: root}); out != "a+b\n" {
		t.Errorf("from the root: output %q, errors %q", out, errs)
	}
	_, errs := run(Config{LibPath: []string{}, IncludePath: []string{}})
	if !strings.Contains(errs, "cannot open include file") && !strings.Contains(errs, "cannot resolve") {
		t.Errorf("with no root: errors %q", errs)
	}
	// A strings.icn on IPATH comes first.
	if err := os.Mkdir("mine", 0o755); err != nil {
		t.Fatal(err)
	}
	mine := "procedure replace(s, a, b)\n   return \"mine\"\nend\n"
	if err := os.WriteFile(filepath.Join("mine", "strings.icn"), []byte(mine), 0o644); err != nil {
		t.Fatal(err)
	}
	if out, errs := run(Config{LibPath: []string{"mine"}, IncludePath: []string{}, Root: root}); out != "mine\n" {
		t.Errorf("shadowed: output %q, errors %q", out, errs)
	}
}

// TestNoRootHint checks the hint when a link fails with no root.
func TestNoRootHint(t *testing.T) {
	t.Chdir(t.TempDir())
	if err := os.WriteFile("p.icn", []byte("link strings\nprocedure main()\nend\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var errs bytes.Buffer
	File("p.icn", Config{Stdout: &errs, Stderr: &errs, LibPath: []string{}, IncludePath: []string{}})
	if !strings.Contains(errs.String(), "no GCONROOT found") {
		t.Errorf("no hint: %q", errs.String())
	}
}
