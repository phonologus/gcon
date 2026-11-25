// Command difftest runs Icon programs under both iconx and gcon and
// compares what they do: standard output, standard error and exit
// status. It is a differential test against the reference
// implementation, for programs that have no recorded expected output,
// such as those of the Icon Program Library.
//
//	go run ./tools/difftest -iconx $(tools/iconx/build.sh) -gcon bin/gcon [-out dir] file.icn...
//
// Each program is built by icont and by gcon, and each built program is
// run as ./NAME in a directory of its own (so &progname and any files
// it writes match), with its own source as standard input and no
// arguments, unless FILE.args or FILE.dat beside it say otherwise. A
// program whose iconx output differs between two runs is reported as
// nondeterministic, not compared. The differences are written to -out.
package main

import (
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

var (
	iconxDir = flag.String("iconx", "", "directory holding the reference icont and iconx (tools/iconx/build.sh)")
	gconPath = flag.String("gcon", "bin/gcon", "the gcon to test")
	outDir   = flag.String("out", "", "directory for the differences (default: a temporary directory)")
	timeout  = flag.Duration("timeout", 10*time.Second, "time limit for each run")
	verbose  = flag.Bool("v", false, "list every program's outcome")
)

// result is what one run did.
type result struct {
	stdout, stderr []byte
	status         int
	timedOut       bool
	elapsed        time.Duration
}

func (r *result) same(s *result) bool {
	return bytes.Equal(r.stdout, s.stdout) && bytes.Equal(r.stderr, s.stderr) && r.status == s.status
}

// Outcomes.
const (
	same       = "same"
	differ     = "DIFFER"
	nondet     = "nondeterministic"
	iconxSlow  = "iconx timeout"
	gconSlow   = "GCON TIMEOUT"
	noIcont    = "icont fails"
	noGcon     = "GCON BUILD FAILS"
	bothFail   = "neither builds"
	setupError = "setup error"
)

type outcome struct {
	name, kind  string
	iconx, gcon time.Duration
	note        string
}

func main() {
	flag.Parse()
	if *iconxDir == "" || flag.NArg() == 0 {
		fmt.Fprintln(os.Stderr, "usage: difftest -iconx DIR [-gcon PATH] [-out DIR] file.icn...")
		os.Exit(2)
	}
	gcon, err := filepath.Abs(*gconPath)
	if err != nil {
		fatal(err)
	}
	icont := filepath.Join(*iconxDir, "icont")
	if *outDir == "" {
		if *outDir, err = os.MkdirTemp("", "difftest"); err != nil {
			fatal(err)
		}
	}
	if err := os.MkdirAll(*outDir, 0o755); err != nil {
		fatal(err)
	}
	work, err := os.MkdirTemp("", "difftest-work")
	if err != nil {
		fatal(err)
	}
	defer os.RemoveAll(work)

	var outcomes []outcome
	for _, f := range flag.Args() {
		o := compare(f, icont, gcon, work)
		outcomes = append(outcomes, o)
		if *verbose || (o.kind != same && o.kind != nondet && o.kind != iconxSlow && o.kind != noIcont) {
			fmt.Printf("%-16s %-18s iconx %7.3fs  gcon %7.3fs  %s\n", o.name, o.kind, o.iconx.Seconds(), o.gcon.Seconds(), o.note)
		}
	}
	counts := map[string]int{}
	for _, o := range outcomes {
		counts[o.kind]++
	}
	kinds := make([]string, 0, len(counts))
	for k := range counts {
		kinds = append(kinds, k)
	}
	sort.Strings(kinds)
	fmt.Printf("\n%d programs:", len(outcomes))
	for _, k := range kinds {
		fmt.Printf("  %s %d", k, counts[k])
	}
	fmt.Printf("\ndifferences in %s\n", *outDir)
	writeTimes(outcomes)
}

// compare builds and runs one program under both implementations.
func compare(src, icont, gcon, work string) outcome {
	name := strings.TrimSuffix(filepath.Base(src), ".icn")
	o := outcome{name: name}
	setup := func(err error) outcome {
		o.kind, o.note = setupError, err.Error()
		return o
	}
	text, err := os.ReadFile(src)
	if err != nil {
		return setup(err)
	}
	stdin := text
	if b, err := os.ReadFile(strings.TrimSuffix(src, ".icn") + ".dat"); err == nil {
		stdin = b
	}
	var args []string
	if b, err := os.ReadFile(strings.TrimSuffix(src, ".icn") + ".args"); err == nil {
		args = strings.Fields(string(b))
	}
	dirs := map[string]string{}
	for _, impl := range []string{"iconx", "gcon", "iconx2"} {
		d := filepath.Join(work, name, impl)
		if err := os.MkdirAll(d, 0o755); err != nil {
			return setup(err)
		}
		if err := os.WriteFile(filepath.Join(d, name+".icn"), text, 0o644); err != nil {
			return setup(err)
		}
		dirs[impl] = d
	}
	// Build. icont finds the IPL's ucode in its own lib directory; gcon
	// finds the IPL's source in its root.
	_, icontErr := run(dirs["iconx"], nil, icont, "-s", "-o", name, name+".icn")
	if icontErr == nil {
		_, icontErr = run(dirs["iconx2"], nil, icont, "-s", "-o", name, name+".icn")
	}
	gconOut, gconErr := run(dirs["gcon"], nil, gcon, "build", "-o", name, name+".icn")
	switch {
	case icontErr != nil && gconErr != nil:
		o.kind = bothFail
		return o
	case icontErr != nil:
		o.kind = noIcont
		return o
	case gconErr != nil:
		o.kind, o.note = noGcon, firstLine(gconOut.stderr)
		return o
	}

	ri, _ := run(dirs["iconx"], stdin, "./"+name, args...)
	rg, _ := run(dirs["gcon"], stdin, "./"+name, args...)
	o.iconx, o.gcon = ri.elapsed, rg.elapsed
	if ri.timedOut {
		o.kind = iconxSlow
		return o
	}
	ri2, _ := run(dirs["iconx2"], stdin, "./"+name, args...)
	switch {
	case !ri.same(ri2):
		o.kind = nondet
	case rg.timedOut:
		o.kind = gconSlow
	case ri.same(rg):
		o.kind = same
	default:
		o.kind = differ
		o.note = summarize(ri, rg)
		writeDiff(name, ri, rg)
	}
	return o
}

// run runs a command in dir with stdin, within the time limit.
func run(dir string, stdin []byte, prog string, args ...string) (*result, error) {
	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, prog, args...)
	cmd.Dir = dir
	cmd.Stdin = bytes.NewReader(stdin)
	cmd.Env = append(os.Environ(), "IPATH=", "LPATH=")
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	start := time.Now()
	err := cmd.Run()
	r := &result{stdout: out.Bytes(), stderr: errb.Bytes(), elapsed: time.Since(start)}
	if ctx.Err() != nil {
		r.timedOut = true
		return r, ctx.Err()
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		r.status = ee.ExitCode()
	} else if err != nil {
		return r, err
	}
	if r.status != 0 {
		return r, fmt.Errorf("exit status %d", r.status)
	}
	return r, nil
}

// summarize says briefly how two runs differ.
func summarize(i, g *result) string {
	var parts []string
	if i.status != g.status {
		parts = append(parts, fmt.Sprintf("status %d vs %d", i.status, g.status))
	}
	if !bytes.Equal(i.stdout, g.stdout) {
		parts = append(parts, "stdout: "+firstDiff(i.stdout, g.stdout))
	}
	if !bytes.Equal(i.stderr, g.stderr) {
		parts = append(parts, "stderr: "+firstDiff(i.stderr, g.stderr))
	}
	return strings.Join(parts, "; ")
}

// firstDiff returns the first differing line of a and b.
func firstDiff(a, b []byte) string {
	la, lb := strings.Split(string(a), "\n"), strings.Split(string(b), "\n")
	for i := 0; i < len(la) || i < len(lb); i++ {
		var x, y string
		if i < len(la) {
			x = la[i]
		}
		if i < len(lb) {
			y = lb[i]
		}
		if x != y {
			return fmt.Sprintf("line %d %q vs %q", i+1, trunc(x), trunc(y))
		}
	}
	return "?"
}

func trunc(s string) string {
	if len(s) > 60 {
		return s[:60] + "..."
	}
	return s
}

func firstLine(b []byte) string {
	s, _, _ := strings.Cut(string(b), "\n")
	return trunc(s)
}

// writeDiff saves both runs' output for a differing program.
func writeDiff(name string, i, g *result) {
	for impl, r := range map[string]*result{"iconx": i, "gcon": g} {
		base := filepath.Join(*outDir, name+"."+impl)
		os.WriteFile(base+".out", r.stdout, 0o644)
		os.WriteFile(base+".err", append(r.stderr, fmt.Sprintf("[exit status %d]\n", r.status)...), 0o644)
	}
}

// writeTimes records each compared program's run times.
func writeTimes(outcomes []outcome) {
	var b strings.Builder
	b.WriteString("program\toutcome\ticonx_s\tgcon_s\n")
	for _, o := range outcomes {
		if o.iconx > 0 {
			fmt.Fprintf(&b, "%s\t%s\t%.4f\t%.4f\n", o.name, o.kind, o.iconx.Seconds(), o.gcon.Seconds())
		}
	}
	os.WriteFile(filepath.Join(*outDir, "times.tsv"), []byte(b.String()), 0o644)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "difftest:", err)
	os.Exit(1)
}
