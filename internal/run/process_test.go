package run

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestStackOverflow checks run-time error 301: runaway recursion stops at
// the evaluation stack's capacity (MSTKSIZE words, two to a descriptor;
// a frame of a procedure with no parameters or locals takes nine), and
// &error does not convert it, as in iconx.
func TestStackOverflow(t *testing.T) {
	t.Setenv("MSTKSIZE", "2000") // 1000 descriptors: 111 such frames
	t.Chdir(t.TempDir())
	src := "procedure r()\n   return r()\nend\nprocedure main()\n   &error := 1\n   r()\n   write(\"converted\")\nend\n"
	if err := os.WriteFile("deep.icn", []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	var out, errs bytes.Buffer
	status := File("deep.icn", Config{Stdout: &out, Stderr: &errs, LibPath: []string{}, IncludePath: []string{}})
	if status != 1 || out.Len() != 0 {
		t.Errorf("status %d, output %q", status, out.String())
	}
	if !strings.HasPrefix(errs.String(), "\nRun-time error 301\nFile deep.icn; Line 2\nevaluation stack overflow\n") {
		t.Errorf("stderr begins %q", strings.SplitAfterN(errs.String(), "\n", 5)[:4])
	}
	// iconx shows 110 calls of r with MSTKSIZE=2000; gcon is within two.
	if n := strings.Count(errs.String(), "\nr() from line"); n < 108 || n > 112 {
		t.Errorf("%d calls in the traceback, want about 110", n)
	}
}

// TestPipeSharesInput checks that a command read through a pipe reads
// gcon's standard input when that is a file, as with popen: here head
// consumes the first line, and gcon then reads the second.
func TestPipeSharesInput(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	src := "procedure main()\n   p := open(\"head -1\", \"p\")\n   write(\"command read: \", read(p))\n   close(p)\n   write(\"gcon read: \", read())\nend\n"
	if err := os.WriteFile("share.icn", []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	in := filepath.Join(dir, "in.txt")
	if err := os.WriteFile(in, []byte("first\nsecond\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	stdin, err := os.Open(in)
	if err != nil {
		t.Fatal(err)
	}
	defer stdin.Close()
	var out, errs bytes.Buffer
	File("share.icn", Config{Stdin: stdin, Stdout: &out, Stderr: &errs, LibPath: []string{}, IncludePath: []string{}})
	if want := "command read: first\ngcon read: second\n"; out.String() != want {
		t.Errorf("output %q, want %q; stderr %q", out.String(), want, errs.String())
	}
}
