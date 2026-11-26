package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/phonologus/gcon/internal/compile"
	"github.com/phonologus/gcon/internal/ipp"
	"github.com/phonologus/gcon/internal/run"
	"github.com/phonologus/gcon/internal/sema"
	"github.com/phonologus/gcon/internal/syntax"
	"github.com/phonologus/gcon/internal/vm"
)

// The build form: the files given are one program (no link resolution),
// each preprocessed (gcon's predefined symbols, the include path in
// $PPPATH), parsed, resolved and compiled as the driver does, and its
// errors, one a line as "File F; Line N # msg", are written; or, if
// there are none, its encoding, with the build information test=1.
func build(names []string) {
	var errs []string
	var files []*sema.File
	for _, name := range names {
		b, err := os.ReadFile(name)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		r := ipp.Preprocess(name, string(b), ipp.Options{
			Predefined:  run.Predefined(),
			IncludePath: strings.Fields(os.Getenv("PPPATH")),
		})
		for _, e := range r.Errors {
			errs = append(errs, e.Error())
		}
		f := syntax.Parse(r.Src)
		for _, e := range f.Errors {
			file, line := r.Map.Locate(e.Offset)
			errs = append(errs, (&compile.Error{File: file, Line: line, Msg: e.Msg}).Error())
		}
		files = append(files, &sema.File{Path: name, Syntax: f, Locate: r.Map.Locate, Preprocessed: true})
	}
	if len(errs) == 0 {
		p := sema.NewProgram(files, true)
		infos := make([]*sema.Info, len(files))
		for i, f := range files {
			infos[i] = p.Resolve(f)
			for _, d := range infos[i].Diagnostics {
				if d.Severity == sema.Error {
					file, line := f.Locate(d.Offset)
					errs = append(errs, (&compile.Error{File: file, Line: line, Msg: d.Msg}).Error())
				}
			}
		}
		if len(errs) == 0 {
			prog, cerrs := compile.Compile(p, infos)
			for _, e := range cerrs {
				errs = append(errs, e.Error())
			}
			if len(errs) == 0 {
				b, err := vm.Encode(prog, map[string]string{"test": "1"})
				if err != nil {
					errs = append(errs, err.Error())
				} else {
					os.Stdout.Write(b)
					return
				}
			}
		}
	}
	for _, e := range errs {
		fmt.Println(esc(e))
	}
	os.Exit(1)
}
