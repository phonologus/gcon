package run

import (
	"bytes"
	"fmt"
	"os"
	"runtime/debug"
	"sort"
	"strings"

	"github.com/phonologus/gcon/internal/vm"
)

// A built program is a shell script that runs gcon on itself, then the
// encoded program (docs/design.md, "Decision: the built program is an
// icont-style script").

// marker ends the shell header, as "[executable Icon binary follows]"
// ends icont's; the encoded program follows it at once.
const marker = "[gcon program follows]\n"

// header returns the shell header of a built program. As icont's, it
// tries, in order: $GCON (which must then work), gcon beside the
// program, the gcon that built it, and gcon on PATH.
func header(gconPath string) string {
	return "#!/bin/sh\n" +
		"GCBIN=" + shellQuote(gconPath) + "\n" +
		"GCLCL=`echo \"$0\" | sed 's=[^/]*$=gcon='`\n" +
		"[ -n \"$GCON\" ] && exec \"$GCON\" run \"$0\" \"$@\"\n" +
		"[ -x \"$GCLCL\" ] && exec \"$GCLCL\" run \"$0\" \"$@\"\n" +
		"[ -x \"$GCBIN\" ] && exec \"$GCBIN\" run \"$0\" \"$@\"\n" +
		"exec gcon run \"$0\" \"$@\"\n" +
		marker
}

// shellQuote quotes s for sh.
func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

// Build returns the built program: the header, naming gconPath as the
// gcon that built it, then prog encoded, with gcon's version and the
// preprocessor's predefined symbols as its build information.
func Build(prog *vm.Program, gconPath string) ([]byte, error) {
	enc, err := vm.Encode(prog, BuildInfo())
	if err != nil {
		return nil, err
	}
	return append([]byte(header(gconPath)), enc...), nil
}

// BuildInfo returns the build information recorded in a built program.
func BuildInfo() vm.Info {
	pre := Predefined()
	syms := make([]string, 0, len(pre))
	for k, v := range pre {
		syms = append(syms, k+"="+v)
	}
	sort.Strings(syms)
	return vm.Info{"gcon": Version(), "predefined": strings.Join(syms, " ")}
}

// Version returns gcon's version: its module version, which for a build
// from a repository is a pseudo-version naming the commit (with
// "+dirty" for uncommitted changes); failing that, the commit.
func Version() string {
	bi, ok := debug.ReadBuildInfo()
	if !ok {
		return "unknown"
	}
	if v := bi.Main.Version; v != "" && v != "(devel)" {
		return v
	}
	var rev, dirty string
	for _, s := range bi.Settings {
		switch s.Key {
		case "vcs.revision":
			rev = s.Value
		case "vcs.modified":
			if s.Value == "true" {
				dirty = "+dirty"
			}
		}
	}
	if rev == "" {
		return "(devel)"
	}
	return "(devel) " + rev + dirty
}

// encoded returns the encoded program within data if data is a built
// program: a file starting "#!" in which a line holding the marker is
// followed by vm.Magic.
func encoded(data []byte) ([]byte, bool) {
	if !bytes.HasPrefix(data, []byte("#!")) {
		return nil, false
	}
	i := bytes.Index(data, []byte("\n"+marker+vm.Magic))
	if i < 0 {
		return nil, false
	}
	return data[i+1+len(marker):], true
}

// IsBuilt reports whether data is a built program.
func IsBuilt(data []byte) bool {
	_, ok := encoded(data)
	return ok
}

// Load returns the program in data, a built program or Icon source read
// from path, with its build information (nil for source).
func Load(path string, data []byte, cfg Config) (*vm.Program, vm.Info, []error) {
	if enc, ok := encoded(data); ok {
		p, info, err := vm.Decode(enc)
		if err != nil {
			return nil, nil, []error{fmt.Errorf("%s: %v", path, err)}
		}
		return p, info, nil
	}
	p, errs := TranslateLinked(path, string(data), cfg.libPath(), cfg.includePath())
	return p, nil, errs
}

// BuildFile translates the program whose main file is path and writes
// it, built, to out, which is made executable: a new file is created
// with mode 0777 less the umask; an existing file without execute bits
// gets one wherever it is readable, as icont's setexe does.
func BuildFile(path, out, gconPath string, cfg Config) []error {
	src, err := os.ReadFile(path)
	if err != nil {
		return []error{err}
	}
	prog, errs := TranslateLinked(path, string(src), cfg.libPath(), cfg.includePath())
	if len(errs) > 0 {
		return errs
	}
	b, err := Build(prog, gconPath)
	if err != nil {
		return []error{err}
	}
	if err := os.WriteFile(out, b, 0o777); err != nil {
		return []error{err}
	}
	st, err := os.Stat(out)
	if err != nil {
		return []error{err}
	}
	if m := st.Mode().Perm(); m&0o111 == 0 {
		if err := os.Chmod(out, m|(m&0o444)>>2); err != nil {
			return []error{err}
		}
	}
	return nil
}
