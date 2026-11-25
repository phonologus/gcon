package run

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/phonologus/gcon/internal/vm"
)

// Running on gcon-vm (docs/design.md, "Decision: gcon run uses
// gcon-vm"): gcon translates and builds; gcon-vm, the VM in C, runs. The
// Go VM remains, as the reference and where gcon-vm is not installed.

// FindVM returns the gcon-vm that gcon run should use, given the root's
// directory, and how it was chosen: $GCONVM if set ("go" chooses the Go
// VM, returned as ""); else libexec/gcon-vm in the root, if it is an
// executable file; else "", the Go VM. gcon-vm runs only where gcon can
// exec it (Unix).
func FindVM(rootDir string) (path, from string) {
	if !canExec {
		return "", "unsupported"
	}
	if v := os.Getenv("GCONVM"); v != "" {
		if v == "go" {
			return "", "GCONVM"
		}
		return v, "GCONVM"
	}
	if rootDir != "" {
		p := filepath.Join(rootDir, "libexec", "gcon-vm")
		if st, err := os.Stat(p); err == nil && st.Mode().IsRegular() && st.Mode()&0o111 != 0 {
			return p, "root"
		}
	}
	return "", "not found"
}

// ExecVM runs the program at path, a built program or Icon source, on
// the gcon-vm at vmPath, with the given arguments, replacing this
// process. A built program is run as it is, named by path. Source is
// translated, built into a file deleted at once (gcon-vm reads it as
// /dev/fd/N), and run with the &progname it would have from source. It
// returns only on failure, with the exit status: translation errors are
// reported as File reports them.
func ExecVM(vmPath, path string, cfg Config) int {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(cfg.Stderr, err)
		return 1
	}
	argv := []string{vmPath, path}
	if !IsBuilt(data) {
		prog, errs := TranslateLinked(path, string(data), cfg.libPath(), cfg.includePath())
		if len(errs) > 0 {
			Report(cfg.Stderr, errs, cfg)
			return 1
		}
		enc, err := vm.Encode(prog, BuildInfo())
		if err != nil {
			fmt.Fprintln(cfg.Stderr, err)
			return 1
		}
		fd, err := tempProgram(enc)
		if err != nil {
			fmt.Fprintf(cfg.Stderr, "gcon: %v\n", err)
			return 1
		}
		argv = []string{vmPath, "-progname", vm.ProgName(path), fmt.Sprintf("/dev/fd/%d", fd)}
	}
	argv = append(argv, cfg.Args...)
	err = execve(vmPath, argv)
	fmt.Fprintf(cfg.Stderr, "gcon: cannot run %s: %v (GCONVM=go uses the Go VM)\n", vmPath, err)
	return 1
}
