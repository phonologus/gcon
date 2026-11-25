// Command gcon runs Icon programs.
//
//	gcon run file [args...]        run Icon source, or a built program
//	gcon build [-o out] file.icn   build a program: an executable file
//	gcon dis file                  list the VM code of source or a built program
//	gcon version                   print gcon's version
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/phonologus/gcon/internal/run"
	"github.com/phonologus/gcon/internal/vm"
)

func usage() {
	fmt.Fprint(os.Stderr, `usage:
	gcon run file [args...]
	gcon build [-o out] file.icn
	gcon dis file
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
		os.Exit(run.File(args[0], run.Config{Args: args[1:], Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr}))
	case "build":
		os.Exit(build(args))
	case "dis":
		os.Exit(dis(args))
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
	if errs := run.BuildFile(src, *out, self, run.Config{}); len(errs) > 0 {
		for _, e := range errs {
			fmt.Fprintln(os.Stderr, e)
		}
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
	prog, info, errs := run.Load(args[0], data, run.Config{})
	if len(errs) > 0 {
		for _, e := range errs {
			fmt.Fprintln(os.Stderr, e)
		}
		return 1
	}
	if err := vm.Disasm(os.Stdout, prog, info); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}
