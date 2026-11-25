package run

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// TestPrograms runs each testdata/*.icn and compares its standard output
// and exit status with those of the reference implementation, recorded
// by tools/iconx/expect.sh in FILE.std and FILE.exit. FILE.dat, if
// present, is standard input; FILE.args holds arguments. Each program
// runs in a directory of its own, so that it may create files.
func TestPrograms(t *testing.T) {
	files, err := filepath.Glob("testdata/*.icn")
	if err != nil || len(files) == 0 {
		t.Fatal("no test programs")
	}
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		base := filepath.Join(wd, strings.TrimSuffix(f, ".icn"))
		t.Run(filepath.Base(base), func(t *testing.T) {
			t.Chdir(t.TempDir())
			want, err := os.ReadFile(base + ".std")
			if err != nil {
				t.Fatal(err)
			}
			wantStatus := 0
			if b, err := os.ReadFile(base + ".exit"); err == nil {
				wantStatus, _ = strconv.Atoi(strings.TrimSpace(string(b)))
			}
			var args []string
			if b, err := os.ReadFile(base + ".args"); err == nil {
				args = strings.Fields(string(b))
			}
			var stdin io.Reader
			if b, err := os.ReadFile(base + ".dat"); err == nil {
				stdin = bytes.NewReader(b)
			}
			var stdout, stderr bytes.Buffer
			status := File(base+".icn", Config{Args: args, Stdin: stdin, Stdout: &stdout, Stderr: &stderr, Timeout: 10 * time.Second})
			if got := stdout.String(); got != string(want) {
				t.Errorf("output differs\n--- got\n%s--- want\n%s--- stderr\n%s", got, want, stderr.String())
			}
			if status != wantStatus {
				t.Errorf("exit status %d, want %d; stderr:\n%s", status, wantStatus, stderr.String())
			}
		})
	}
}
