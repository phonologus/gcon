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
	"github.com/phonologus/gcon/internal/lexer"
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
}

// EnvLibPath returns the directories of IPATH, which may be separated by
// spaces or colons, as icont's are.
func EnvLibPath() []string {
	return strings.FieldsFunc(os.Getenv("IPATH"), func(r rune) bool { return r == ':' || r == ' ' })
}

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
	prog, errs := TranslateLinked(path, string(src), lib)
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
	return TranslateLinked(path, src, EnvLibPath())
}

// unit is one source file of a program.
type unit struct {
	path  string // where it was read from
	name  string // its name in the program: path for the main file, the link name + ".icn" for others
	src   string
	file  *syntax.File
	lines *lexer.Lines
}

// TranslateLinked parses the program whose main file is path (with
// source src) and the files it links, transitively, then resolves and
// compiles them together (docs/design.md, "Linking"). Linked files are
// found as findLink finds them; each is read once. A linked file is
// known by the name it was linked by, with ".icn" (so a library's
// procedures report "printf.icn"), as icont knows a file by the name it
// was translated under.
func TranslateLinked(path, src string, libPath []string) (*vm.Program, []error) {
	units := []*unit{{path: path, name: path, src: src}}
	seen := map[string]bool{absPath(path): true}
	var errs []error
	errorf := func(u *unit, off int, format string, args ...any) {
		line, _ := u.lines.Position(off)
		errs = append(errs, &compile.Error{File: u.name, Line: line, Msg: fmt.Sprintf(format, args...)})
	}
	for i := 0; i < len(units); i++ {
		u := units[i]
		u.file = syntax.Parse(u.src)
		u.lines = lexer.NewLines(u.src)
		for _, e := range u.file.Errors {
			errorf(u, e.Offset, "%s", e.Msg)
		}
		for j := range u.file.Tokens {
			for _, tr := range u.file.Tokens[j].Leading {
				if tr.Kind == lexer.Directive {
					errorf(u, tr.Offset, "the preprocessor is not yet supported")
				}
			}
		}
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
				found, ok := findLink(name, libPath)
				if !ok {
					errorf(u, t.Offset, "cannot resolve reference to file '%s.icn'", name)
					continue
				}
				if seen[absPath(found)] {
					continue
				}
				seen[absPath(found)] = true
				b, err := os.ReadFile(found)
				if err != nil {
					errorf(u, t.Offset, "%v", err)
					continue
				}
				units = append(units, &unit{path: found, name: name + ".icn", src: string(b)})
			}
		}
	}
	if len(errs) > 0 {
		return nil, errs
	}
	files := make([]*sema.File, len(units))
	for i, u := range units {
		files[i] = &sema.File{Path: u.name, Syntax: u.file}
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

// findLink finds the source of "link name": name.icn in the current
// directory, else in each directory of libPath in turn, as icont's
// pathfind finds name.u1.
func findLink(name string, libPath []string) (string, bool) {
	file := name + ".icn"
	if fileExists(file) {
		return file, true
	}
	if filepath.IsAbs(file) {
		return "", false
	}
	for _, dir := range libPath {
		if p := filepath.Join(dir, file); fileExists(p) {
			return p, true
		}
	}
	return "", false
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
