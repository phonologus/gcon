// Package root finds gcon's root directory, GCONROOT, which holds the
// Icon Program Library's source (lib/ipl) and, later, the display server
// (libexec): docs/design.md, "Decision: a gcon root, as GOROOT".
package root

import (
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
)

// ModulePath is gcon's module path, as the module cache knows it.
const ModulePath = "github.com/phonologus/gcon"

// defaultRoot is the root a packager names at link time:
//
//	go build -ldflags "-X github.com/phonologus/gcon/internal/root.defaultRoot=/usr/local/share/gcon" ./cmd/gcon
var defaultRoot string

// Source says how the root was found.
type Source string

const (
	FromEnv        Source = "GCONROOT"
	FromExecutable Source = "executable"
	FromDefault    Source = "default"
	FromModCache   Source = "module cache"
	NotFound       Source = "not found"
)

// Root is a gcon root.
type Root struct {
	Dir    string // "" if not found
	Source Source
}

// Library directories within a root.
var (
	LinkDirs    = []string{"lib/ipl/procs", "lib/ipl/gprocs"}
	IncludeDirs = []string{"lib/ipl/incl", "lib/ipl/gincl"}
)

// LinkPath returns the directories link searches in the root, after the
// current directory and IPATH.
func (r Root) LinkPath() []string { return r.join(LinkDirs) }

// IncludePath returns the directories $include searches in the root,
// after the current directory and LPATH.
func (r Root) IncludePath() []string { return r.join(IncludeDirs) }

func (r Root) join(dirs []string) []string {
	if r.Dir == "" {
		return nil
	}
	p := make([]string, len(dirs))
	for i, d := range dirs {
		p[i] = filepath.Join(r.Dir, filepath.FromSlash(d))
	}
	return p
}

// Find returns the root, found as Go finds GOROOT: $GCONROOT if set
// (used as it is); else the directory above the executable, after
// following symbolic links, if it holds the library (an unpacked release,
// or a repository checkout with gcon built into bin); else the default
// set at link time; else gcon's own module in the module cache, which
// `go install` has filled.
func Find() Root {
	exe, err := os.Executable()
	if err == nil {
		if e, err := filepath.EvalSymlinks(exe); err == nil {
			exe = e
		}
	} else {
		exe = ""
	}
	return find(env{
		getenv:   os.Getenv,
		exe:      exe,
		def:      defaultRoot,
		buildMod: buildModule(),
		isRoot:   IsRoot,
	})
}

// env is what find consults, so that tests can supply it.
type env struct {
	getenv   func(string) string
	exe      string    // the executable, symbolic links followed; "" if unknown
	def      string    // defaultRoot
	buildMod [2]string // the main module's path and version
	isRoot   func(dir string) bool
}

func find(e env) Root {
	if d := e.getenv("GCONROOT"); d != "" {
		return Root{d, FromEnv}
	}
	if e.exe != "" {
		if d := filepath.Dir(filepath.Dir(e.exe)); e.isRoot(d) {
			return Root{d, FromExecutable}
		}
	}
	if e.def != "" && e.isRoot(e.def) {
		return Root{e.def, FromDefault}
	}
	if path, version := e.buildMod[0], e.buildMod[1]; path == ModulePath && version != "" && version != "(devel)" {
		if cache := modCache(e.getenv); cache != "" {
			if d := filepath.Join(cache, escape(path)+"@"+escape(version)); e.isRoot(d) {
				return Root{d, FromModCache}
			}
		}
	}
	return Root{"", NotFound}
}

// IsRoot reports whether dir holds the library.
func IsRoot(dir string) bool {
	st, err := os.Stat(filepath.Join(dir, "lib", "ipl", "procs"))
	return err == nil && st.IsDir()
}

// buildModule returns the main module's path and version: for a gcon
// built by `go install github.com/phonologus/gcon/cmd/gcon@v1.2.3`,
// ModulePath and "v1.2.3".
func buildModule() [2]string {
	bi, ok := debug.ReadBuildInfo()
	if !ok {
		return [2]string{}
	}
	return [2]string{bi.Main.Path, bi.Main.Version}
}

// modCache returns the module cache directory as the go command finds
// it: $GOMODCACHE, else the first directory of $GOPATH plus /pkg/mod,
// else $HOME/go/pkg/mod.
func modCache(getenv func(string) string) string {
	if d := getenv("GOMODCACHE"); d != "" {
		return d
	}
	if p := getenv("GOPATH"); p != "" {
		if first := filepath.SplitList(p)[0]; first != "" {
			return filepath.Join(first, "pkg", "mod")
		}
	}
	if h := getenv("HOME"); h != "" {
		return filepath.Join(h, "go", "pkg", "mod")
	}
	return ""
}

// escape escapes a module path or version as the module cache does:
// each upper-case letter becomes '!' and its lower case.
func escape(s string) string {
	var b strings.Builder
	for _, r := range s {
		if 'A' <= r && r <= 'Z' {
			b.WriteByte('!')
			r += 'a' - 'A'
		}
		b.WriteRune(r)
	}
	return b.String()
}
