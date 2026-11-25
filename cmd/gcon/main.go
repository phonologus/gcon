// Command gcon runs Icon programs.
//
//	gcon run file.icn [args...]
package main

import (
	"fmt"
	"os"

	"github.com/phonologus/gcon/internal/run"
)

func usage() {
	fmt.Fprintln(os.Stderr, "usage: gcon run file.icn [args...]")
	os.Exit(2)
}

func main() {
	if len(os.Args) < 3 || os.Args[1] != "run" {
		usage()
	}
	os.Exit(run.File(os.Args[2], run.Config{Args: os.Args[3:], Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr}))
}
