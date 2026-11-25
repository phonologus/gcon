package run

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func TestFindVM(t *testing.T) {
	if !canExec {
		t.Skip("gcon-vm is not used on this system")
	}
	root := t.TempDir()
	t.Setenv("GCONVM", "")
	if p, from := FindVM(root); p != "" || from != "not found" {
		t.Errorf("empty root: %q, %q", p, from)
	}
	vm := filepath.Join(root, "libexec", "gcon-vm")
	os.MkdirAll(filepath.Dir(vm), 0o755)
	os.WriteFile(vm, []byte("#!/bin/sh\n"), 0o644)
	if p, _ := FindVM(root); p != "" {
		t.Errorf("not executable: %q", p)
	}
	os.Chmod(vm, 0o755)
	if p, from := FindVM(root); p != vm || from != "root" {
		t.Errorf("root: %q, %q", p, from)
	}
	t.Setenv("GCONVM", "/elsewhere/gcon-vm")
	if p, from := FindVM(root); p != "/elsewhere/gcon-vm" || from != "GCONVM" {
		t.Errorf("GCONVM: %q, %q", p, from)
	}
	t.Setenv("GCONVM", "go")
	if p, from := FindVM(root); p != "" || from != "GCONVM" {
		t.Errorf("GCONVM=go: %q, %q", p, from)
	}
}

// TestExecVM runs each test program, from source, as gcon run does on
// gcon-vm (ExecVM: translated, built to a deleted file, exec'd), and
// compares its output, exit status and, where recorded, standard error
// with iconx's. It needs gcon-vm: GCONVM names it, else it is skipped.
// ExecVM replaces the process, so each program runs in a child: this
// test binary, re-run as a helper (TestExecVMHelper).
func TestExecVM(t *testing.T) {
	vmPath := os.Getenv("GCONVM")
	if vmPath == "" || vmPath == "go" || !canExec {
		t.Skip("GCONVM does not name a gcon-vm")
	}
	files, _ := filepath.Glob("testdata/*.icn")
	more, _ := filepath.Glob("testdata/traceback/*.icn")
	files = append(files, more...)
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		base := filepath.Join(wd, strings.TrimSuffix(f, ".icn"))
		t.Run(filepath.Base(base), func(t *testing.T) {
			dir := t.TempDir()
			name := filepath.Base(base) + ".icn"
			src, err := os.ReadFile(base + ".icn")
			if err != nil {
				t.Fatal(err)
			}
			os.WriteFile(filepath.Join(dir, name), src, 0o644)
			if err := os.CopyFS(dir, os.DirFS(filepath.Join(wd, "testdata", "lib"))); err != nil {
				t.Fatal(err)
			}
			want, err := os.ReadFile(base + ".std")
			if err != nil {
				t.Fatal(err)
			}
			wantStatus := 0
			if b, err := os.ReadFile(base + ".exit"); err == nil {
				wantStatus, _ = strconv.Atoi(strings.TrimSpace(string(b)))
			}
			args := []string{"-test.run=^TestExecVMHelper$", "--", vmPath, name}
			if b, err := os.ReadFile(base + ".args"); err == nil {
				args = append(args, strings.Fields(string(b))...)
			}
			cmd := exec.Command(os.Args[0], args...)
			cmd.Dir = dir
			cmd.Env = append(os.Environ(), "GCON_EXECVM_HELPER=1")
			if b, err := os.ReadFile(base + ".dat"); err == nil {
				cmd.Stdin = bytes.NewReader(b)
			}
			var stdout, stderr bytes.Buffer
			cmd.Stdout, cmd.Stderr = &stdout, &stderr
			status := 0
			if err := cmd.Run(); err != nil {
				var ee *exec.ExitError
				if !errors.As(err, &ee) {
					t.Fatal(err)
				}
				status = ee.ExitCode()
			}
			if _, err := os.Stat(base + ".err"); err != nil && wantStatus == 1 && strings.Contains(stderr.String(), "# ") {
				return // a translation-error test: File checks its messages
			}
			if got := stdout.String(); got != string(want) {
				t.Errorf("output differs\n--- got\n%s--- want\n%s--- stderr\n%s", got, want, stderr.String())
			}
			if wantErr, err := os.ReadFile(base + ".err"); err == nil && stderr.String() != string(wantErr) {
				t.Errorf("standard error differs\n--- got\n%s--- want\n%s", stderr.String(), wantErr)
			}
			if status != wantStatus {
				t.Errorf("exit status %d, want %d; stderr:\n%s", status, wantStatus, stderr.String())
			}
		})
	}
}

// TestExecVMHelper is TestExecVM's child: it runs ExecVM on the arguments
// after "--" (gcon-vm, the program, its arguments).
func TestExecVMHelper(t *testing.T) {
	if os.Getenv("GCON_EXECVM_HELPER") == "" {
		t.Skip("run by TestExecVM")
	}
	i := 0
	for i < len(os.Args) && os.Args[i] != "--" {
		i++
	}
	a := os.Args[i+1:]
	cfg := Config{Args: a[2:], Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr,
		LibPath: []string{}, IncludePath: []string{}}
	os.Exit(ExecVM(a[0], a[1], cfg))
}

// TestHeaderChoice runs a built program's header with stand-ins for
// gcon and gcon-vm that report how they were run, checking the order in
// which the header chooses (see header).
func TestHeaderChoice(t *testing.T) {
	if !canExec {
		t.Skip("gcon-vm is not used on this system")
	}
	dir := t.TempDir()
	script := func(name, who string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte("#!/bin/sh\necho "+who+" \"$@\"\n"), 0o755); err != nil {
			t.Fatal(err)
		}
		return p
	}
	gcon, vm := script("gcon", "gcon"), script("gcon-vm", "vm")
	other, envgcon := script("other-vm", "other"), script("env-gcon", "envgcon")
	prog, errs := TranslateLinked("p.icn", "procedure main()\nend\n", []string{}, []string{})
	if len(errs) > 0 {
		t.Fatal(errs)
	}
	b, err := Build(prog, gcon, vm)
	if err != nil {
		t.Fatal(err)
	}
	os.Mkdir(filepath.Join(dir, "sub"), 0o755)
	if err := os.WriteFile(filepath.Join(dir, "sub", "prog"), b, 0o755); err != nil {
		t.Fatal(err)
	}
	run := func(env ...string) string {
		cmd := exec.Command("./sub/prog", "a")
		cmd.Dir = dir
		cmd.Env = append([]string{"PATH=/usr/bin:/bin"}, env...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("%v: %v\n%s", env, err, out)
		}
		return strings.TrimSpace(string(out))
	}
	for _, c := range []struct {
		env  []string
		want string
	}{
		{nil, "vm ./sub/prog a"},
		{[]string{"GCONVM=go"}, "gcon run ./sub/prog a"},
		{[]string{"GCONVM=" + other}, "other ./sub/prog a"},
		{[]string{"GCON=" + envgcon}, "envgcon run ./sub/prog a"},
	} {
		if got := run(c.env...); got != c.want {
			t.Errorf("%v: got %q, want %q", c.env, got, c.want)
		}
	}
	// A gcon beside the program decides.
	local := filepath.Join(dir, "sub", "gcon")
	os.WriteFile(local, []byte("#!/bin/sh\necho local \"$@\"\n"), 0o755)
	if got := run(); got != "local run ./sub/prog a" {
		t.Errorf("gcon beside: got %q", got)
	}
	os.Remove(local)
	// No gcon-vm in the root: through gcon.
	os.Chmod(vm, 0o644)
	if got := run(); got != "gcon run ./sub/prog a" {
		t.Errorf("no gcon-vm: got %q", got)
	}
}
