// Command gcon runs Icon programs.
//
//	gcon run file [args...]        run Icon source, or a built program
//	gcon build [-o out] file.icn   build a program: an executable file
//	gcon dis file                  list the VM code of source or a built program
//	gcon env                       print gcon's root and library search paths
//	gcon version                   print gcon's version
//
// On Unix, run hands programs to gcon-vm, the VM in C, when it is
// installed in the root's libexec or named by $GCONVM; GCONVM=go, or no
// gcon-vm, runs them on gcon's own VM.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/phonologus/gcon/internal/root"
	"github.com/phonologus/gcon/internal/run"
	"github.com/phonologus/gcon/internal/vm"
)

func usage() {
	fmt.Fprint(os.Stderr, `usage:
	gcon run file [args...]
	gcon build [-o out] file.icn
	gcon dis file
	gcon env
	gcon version
`)
	os.Exit(2)
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	cmd, args := os.Args[1], os.Args[2:]
	switch cmd {
	case "run":
		if len(args) < 1 {
			usage()
		}
		cfg := config()
		cfg.Args, cfg.Stdin, cfg.Stdout, cfg.Stderr = args[1:], os.Stdin, os.Stdout, os.Stderr
		if vmPath, _ := run.FindVM(gconRoot.Dir); vmPath != "" {
			os.Exit(run.ExecVM(vmPath, args[0], cfg)) // returns only on failure
		}
		os.Exit(run.File(args[0], cfg))
	case "build":
		os.Exit(build(args))
	case "dis":
		os.Exit(dis(args))
	case "env":
		env()
	case "version":
		fmt.Println("gcon", run.Version())
	default:
		usage()
	}
}

// build writes the built program, by default named as icont names its
// output: the main file's name without ".icn", in the current directory.
func build(args []string) int {
	fs := flag.NewFlagSet("build", flag.ExitOnError)
	out := fs.String("o", "", "write the built program to `file`")
	fs.Usage = usage
	fs.Parse(args)
	if fs.NArg() != 1 {
		usage()
	}
	src := fs.Arg(0)
	if *out == "" {
		*out = strings.TrimSuffix(filepath.Base(src), ".icn")
		if *out == filepath.Base(src) {
			fmt.Fprintf(os.Stderr, "gcon build: %s: name the output with -o\n", src)
			return 2
		}
	}
	self, err := os.Executable()
	if err != nil {
		self = "gcon"
	}
	if errs := run.BuildFile(src, *out, self, config()); len(errs) > 0 {
		report(errs)
		return 1
	}
	return 0
}

func dis(args []string) int {
	if len(args) != 1 {
		usage()
	}
	data, err := os.ReadFile(args[0])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	prog, info, errs := run.Load(args[0], data, config())
	if len(errs) > 0 {
		report(errs)
		return 1
	}
	if err := vm.Disasm(os.Stdout, prog, info); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

// gconRoot is gcon's root, found once.
var gconRoot = root.Find()

// config returns the configuration for translating: the library search
// paths of the environment, then the root's library.
func config() run.Config { return run.Config{Root: gconRoot.Dir} }

// report writes translation errors.
func report(errs []error) { run.Report(os.Stderr, errs, config()) }

// env prints gcon's root and search paths, as go env prints Go's, in a
// form sh can read.
func env() {
	cfg := config()
	q := func(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }
	fmt.Printf("GCONROOT=%s\n", q(gconRoot.Dir))
	fmt.Printf("GCONROOTFROM=%s\n", q(string(gconRoot.Source)))
	fmt.Printf("IPATH=%s\n", q(os.Getenv("IPATH")))
	fmt.Printf("LPATH=%s\n", q(os.Getenv("LPATH")))
	fmt.Printf("GCONLINKPATH=%s\n", q(strings.Join(cfg.LinkSearch(), ":")))
	fmt.Printf("GCONINCLUDEPATH=%s\n", q(strings.Join(cfg.IncludeSearch(), ":")))
	vmPath, vmFrom := run.FindVM(gconRoot.Dir)
	fmt.Printf("GCONVM=%s\n", q(vmPath))
	fmt.Printf("GCONVMFROM=%s\n", q(vmFrom))
	fmt.Printf("GCONVERSION=%s\n", q(run.Version()))
}
