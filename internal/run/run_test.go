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
// by tools/iconx/expect.sh in FILE.std and FILE.exit, and its standard
// error with FILE.err if that exists (programs marked "#STDERR"). The
// program runs as FILE.icn from its directory, as expect.sh runs it,
// since its name appears in trace and error messages. FILE.dat, if
// present, is standard input; FILE.args holds arguments. Each program
// runs in a directory of its own, so that it may create files.
func TestPrograms(t *testing.T) {
	files, err := filepath.Glob("testdata/*.icn")
	if err != nil || len(files) == 0 {
		t.Fatal("no test programs")
	}
	// testdata/traceback: run-time errors, checking the error message
	// and traceback.
	more, _ := filepath.Glob("testdata/traceback/*.icn")
	files = append(files, more...)
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
			src, err := os.ReadFile(base + ".icn")
			if err != nil {
				t.Fatal(err)
			}
			name := filepath.Base(base) + ".icn"
			if err := os.WriteFile(name, src, 0o644); err != nil {
				t.Fatal(err)
			}
			// Files the programs link or include are in testdata/lib,
			// copied beside the program as expect.sh copies them for
			// icont, so that they are found, and named, the same way.
			if err := os.CopyFS(".", os.DirFS(filepath.Join(wd, "testdata", "lib"))); err != nil {
				t.Fatal(err)
			}
			status := File(name, Config{Args: args, Stdin: stdin, Stdout: &stdout, Stderr: &stderr,
				Timeout: 10 * time.Second, LibPath: []string{}, IncludePath: []string{}})
			if got := stdout.String(); got != string(want) {
				t.Errorf("output differs\n--- got\n%s--- want\n%s--- stderr\n%s", got, want, stderr.String())
			}
			if wantErr, err := os.ReadFile(base + ".err"); err == nil {
				if got := stderr.String(); got != string(wantErr) {
					t.Errorf("standard error differs\n--- got\n%s--- want\n%s", got, wantErr)
				}
			}
			if status != wantStatus {
				t.Errorf("exit status %d, want %d; stderr:\n%s", status, wantStatus, stderr.String())
			}
		})
	}
}
