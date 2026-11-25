// Package run translates and runs an Icon program.
package run

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/phonologus/gcon/internal/compile"
	"github.com/phonologus/gcon/internal/ipp"
	"github.com/phonologus/gcon/internal/lit"
	"github.com/phonologus/gcon/internal/sema"
	"github.com/phonologus/gcon/internal/syntax"
	"github.com/phonologus/gcon/internal/token"
	"github.com/phonologus/gcon/internal/vm"
)

// Config says how to run a program.
type Config struct {
	Args           []string
	Stdin          io.Reader // if nil, empty
	Stdout, Stderr io.Writer
	Timeout        time.Duration // if nonzero, stop the program after this long (for tests)
	// LibPath lists the directories link searches after the current
	// directory; nil means those of the environment variable IPATH.
	LibPath []string
	// IncludePath lists the directories $include searches after the
	// current directory; nil means those of LPATH.
	IncludePath []string
}

// envPath splits the directories of a search-path variable, which may be
// separated by spaces or colons, as icont's are.
func envPath(name string) []string {
	return strings.FieldsFunc(os.Getenv(name), func(r rune) bool { return r == ':' || r == ' ' })
}

// featureSymbols are the preprocessor symbols of icont's features
// (src/h/features.h), by the &features string.
var featureSymbols = map[string]string{
	"UNIX": "_UNIX", "Macintosh": "_MACINTOSH", "MS Windows": "_MS_WINDOWS", "ASCII": "_ASCII",
	"co-expressions": "_CO_EXPRESSIONS", "keyboard functions": "_KEYBOARD_FUNCTIONS",
	"large integers": "_LARGE_INTEGERS", "pipes": "_PIPES",
	"system function": "_SYSTEM_FUNCTION",
}

// Predefined returns the preprocessor's predefined symbols: _V9, and one
// for each of gcon's &features, each defined as 1, as icont's are.
func Predefined() map[string]string {
	m := map[string]string{"_V9": "1"}
	for _, f := range vm.Features {
		if s := featureSymbols[f]; s != "" {
			m[s] = "1"
		}
	}
	return m
}

// EnvLibPath returns the directories of IPATH.
func EnvLibPath() []string { return envPath("IPATH") }

// File translates the Icon source file at path, with the files it links,
// and runs it, returning the exit status. Translation errors are written
// to stderr, as icont writes them, with status 1.
func File(path string, cfg Config) int {
	src, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(cfg.Stderr, err)
		return 1
	}
	lib := cfg.LibPath
	if lib == nil {
		lib = EnvLibPath()
	}
	incl := cfg.IncludePath
	if incl == nil {
		incl = envPath("LPATH")
	}
	prog, errs := TranslateLinked(path, string(src), lib, incl)
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

// Translate parses, resolves and compiles one source file, finding the
// files it links on IPATH.
func Translate(path, src string) (*vm.Program, []error) {
	return TranslateLinked(path, src, EnvLibPath(), envPath("LPATH"))
}

// unit is one source file of a program.
type unit struct {
	path string // where it was read from
	name string // its name in the program: path for the main file, the link name + ".icn" for others
	file *syntax.File
	pp   *ipp.Map // where the preprocessed text came from
	errs []error  // its preprocessing and syntax errors
}

// load preprocesses and parses the source src of a file known as name.
// Each file is preprocessed on its own, from the predefined symbols, as
// icont translates each file on its own.
func load(path, name, src string, predef map[string]string, includePath []string) *unit {
	u := &unit{path: path, name: name}
	r := ipp.Preprocess(name, src, ipp.Options{Predefined: predef, IncludePath: includePath})
	for _, e := range r.Errors {
		u.errs = append(u.errs, e)
	}
	u.pp = r.Map
	u.file = syntax.Parse(r.Src)
	for _, e := range u.file.Errors {
		file, line := u.pp.Locate(e.Offset)
		u.errs = append(u.errs, &compile.Error{File: file, Line: line, Msg: e.Msg})
	}
	return u
}

// hasMain reports whether u declares a procedure main.
func (u *unit) hasMain() bool {
	for _, d := range u.file.Decls {
		if p, ok := d.(*syntax.ProcDecl); ok && p.Name != nil && p.Name.Text == "main" {
			return true
		}
	}
	return false
}

// TranslateLinked parses the program whose main file is path (with
// source src) and the files it links, transitively, then resolves and
// compiles them together (docs/design.md, "Linking"). Linked files are
// found as findLink finds them; each is read once. A linked file is
// known by the name it was linked by, with ".icn" (so a library's
// procedures report "printf.icn"), as icont knows a file by the name it
// was translated under.
func TranslateLinked(path, src string, libPath, includePath []string) (*vm.Program, []error) {
	predef := Predefined()
	units := []*unit{load(path, path, src, predef, includePath)}
	seen := map[string]bool{absPath(path): true}
	var errs []error
	errorf := func(u *unit, off int, format string, args ...any) {
		file, line := u.pp.Locate(off)
		errs = append(errs, &compile.Error{File: file, Line: line, Msg: fmt.Sprintf(format, args...)})
	}
	for i := 0; i < len(units); i++ {
		u := units[i]
		errs = append(errs, u.errs...)
		for _, d := range u.file.Decls {
			ld, ok := d.(*syntax.LinkDecl)
			if !ok {
				continue
			}
			for _, t := range ld.Files.Elems {
				if t == nil {
					continue
				}
				name := t.Text
				if t.Kind == token.STRINGLIT {
					name, _, _ = lit.String(name)
				}
				// The first file found that is not a program: a file
				// declaring main cannot be linked into a program (it
				// would redeclare main), so the search passes over it,
				// as icont passes over a source file that was never
				// translated to ucode.
				var found *unit
				for _, cand := range findLink(name, libPath) {
					if seen[absPath(cand)] {
						found = &unit{} // already part of the program
						break
					}
					b, err := os.ReadFile(cand)
					if err != nil {
						continue
					}
					c := load(cand, name+".icn", string(b), predef, includePath)
					if !c.hasMain() {
						found = c
						break
					}
				}
				switch {
				case found == nil:
					errorf(u, t.Offset, "cannot resolve reference to file '%s.icn'", name)
				case found.file != nil:
					seen[absPath(found.path)] = true
					units = append(units, found)
				}
			}
		}
	}
	if len(errs) > 0 {
		return nil, errs
	}
	files := make([]*sema.File, len(units))
	for i, u := range units {
		files[i] = &sema.File{Path: u.name, Syntax: u.file, Locate: u.pp.Locate, Preprocessed: true}
	}
	p := sema.NewProgram(files, true)
	infos := make([]*sema.Info, len(units))
	for i, u := range units {
		infos[i] = p.Resolve(files[i])
		for _, d := range infos[i].Diagnostics {
			if d.Severity == sema.Error {
				errorf(u, d.Offset, "%s", d.Msg)
			}
		}
	}
	if len(errs) > 0 {
		return nil, errs
	}
	return compile.Compile(p, infos)
}

// findLink returns the candidates for "link name", in search order:
// name.icn in the current directory, then in each directory of libPath,
// as icont's pathfind searches for name.u1.
func findLink(name string, libPath []string) []string {
	file := name + ".icn"
	var c []string
	if fileExists(file) {
		c = append(c, file)
	}
	if filepath.IsAbs(file) {
		return c
	}
	for _, dir := range libPath {
		if p := filepath.Join(dir, file); fileExists(p) {
			c = append(c, p)
		}
	}
	return c
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func absPath(p string) string {
	if a, err := filepath.Abs(p); err == nil {
		return a
	}
	return p
}
