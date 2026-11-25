package run

import (
	"bytes"
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
// present, is standard input; FILE.args holds arguments.
func TestPrograms(t *testing.T) {
	files, err := filepath.Glob("testdata/*.icn")
	if err != nil || len(files) == 0 {
		t.Fatal("no test programs")
	}
	for _, f := range files {
		base := strings.TrimSuffix(f, ".icn")
		t.Run(filepath.Base(base), func(t *testing.T) {
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
			var stdout, stderr bytes.Buffer
			status := File(f, Config{Args: args, Stdout: &stdout, Stderr: &stderr, Timeout: 10 * time.Second})
			if got := stdout.String(); got != string(want) {
				t.Errorf("output differs\n--- got\n%s--- want\n%s--- stderr\n%s", got, want, stderr.String())
			}
			if status != wantStatus {
				t.Errorf("exit status %d, want %d; stderr:\n%s", status, wantStatus, stderr.String())
			}
		})
	}
}
