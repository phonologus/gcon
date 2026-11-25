// Package run translates and runs an Icon program.
package run

import (
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/phonologus/gcon/internal/compile"
	"github.com/phonologus/gcon/internal/lexer"
	"github.com/phonologus/gcon/internal/sema"
	"github.com/phonologus/gcon/internal/syntax"
	"github.com/phonologus/gcon/internal/vm"
)

// Config says how to run a program.
type Config struct {
	Args           []string
	Stdin          io.Reader // if nil, empty
	Stdout, Stderr io.Writer
	Timeout        time.Duration // if nonzero, stop the program after this long (for tests)
}

// File translates the Icon source file at path and runs it, returning the
// exit status. Translation errors are written to stderr, as icont writes
// them, with status 1.
func File(path string, cfg Config) int {
	src, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(cfg.Stderr, err)
		return 1
	}
	prog, errs := Translate(path, string(src))
	if len(errs) > 0 {
		for _, e := range errs {
			fmt.Fprintln(cfg.Stderr, e)
		}
		return 1
	}
	stdin := cfg.Stdin
	if stdin == nil {
		stdin = strings.NewReader("")
	}
	m := vm.New(prog, stdin, cfg.Stdout, cfg.Stderr)
	m.SetProgname(path)
	if cfg.Timeout > 0 {
		m.Deadline = time.Now().Add(cfg.Timeout)
	}
	return m.Run(cfg.Args)
}

// Translate parses, resolves and compiles one source file.
func Translate(path, src string) (*vm.Program, []error) {
	f := syntax.Parse(src)
	lines := lexer.NewLines(src)
	var errs []error
	for _, e := range f.Errors {
		line, _ := lines.Position(e.Offset)
		errs = append(errs, &compile.Error{File: path, Line: line, Msg: e.Msg})
	}
	for i := range f.Tokens {
		for _, tr := range f.Tokens[i].Leading {
			if tr.Kind == lexer.Directive {
				line, _ := lines.Position(tr.Offset)
				errs = append(errs, &compile.Error{File: path, Line: line,
					Msg: "the preprocessor is not yet supported"})
			}
		}
	}
	if len(errs) > 0 {
		return nil, errs
	}
	sf := &sema.File{Path: path, Syntax: f}
	p := sema.NewProgram([]*sema.File{sf}, true)
	info := p.Resolve(sf)
	for _, d := range info.Diagnostics {
		if d.Severity == sema.Error {
			line, _ := lines.Position(d.Offset)
			errs = append(errs, &compile.Error{File: path, Line: line, Msg: d.Msg})
		}
	}
	if len(errs) > 0 {
		return nil, errs
	}
	return compile.Compile(p, []*sema.Info{info})
}
