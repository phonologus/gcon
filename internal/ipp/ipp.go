// Package ipp is the Icon preprocessor, as icont's (src/common/ipp.c).
//
// It recognises, on lines whose first non-blank character is $ (not
// followed by punctuation other than a space, so that $( and the other
// digraphs are left alone):
//
//	$define name text    $undef name
//	$ifdef name          $ifndef name     $else     $endif
//	$include filename    $line n [filename]          $error [text]
//
// and #line n "filename" at the start of a line. Each identifier outside
// string and cset literals, numbers and comments that names a definition
// is replaced by its text, which is scanned again (a definition is not
// expanded within itself). Directive lines and skipped lines become
// empty, so the output has one line for each line read; Map records,
// for each output line, the file and line it came from, which is how
// positions survive $include.
package ipp

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Options configure a run of the preprocessor.
type Options struct {
	// Predefined are the symbols defined before the file is read (as
	// icont defines those of its features, each as "1").
	Predefined map[string]string
	// IncludePath lists the directories $include searches after the
	// current directory (icont's LPATH).
	IncludePath []string
	// ReadFile reads an included file; nil means os.ReadFile.
	ReadFile func(name string) ([]byte, error)
}

// Error is a preprocessing error, reported as icont reports them.
type Error struct {
	File string
	Line int
	Msg  string
}

func (e *Error) Error() string { return fmt.Sprintf("File %s; Line %d # %s", e.File, e.Line, e.Msg) }

// Pos is a source position: a file and a 1-based line.
type Pos struct {
	File string
	Line int
}

// Map maps byte offsets in the preprocessor's output to source
// positions.
type Map struct {
	starts []int // byte offset of each output line
	pos    []Pos // where each output line came from
}

// Locate returns the file and line of byte offset off in the output.
func (m *Map) Locate(off int) (file string, line int) {
	lo, hi := 0, len(m.starts)
	for lo < hi {
		mid := (lo + hi) / 2
		if m.starts[mid] <= off {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	if lo == 0 {
		if len(m.pos) == 0 {
			return "", 1
		}
		return m.pos[0].File, m.pos[0].Line
	}
	p := m.pos[lo-1]
	return p.File, p.Line
}

// Result is the preprocessor's output.
type Result struct {
	Src    string
	Map    *Map
	Errors []*Error
}

// infile is a file being read.
type infile struct {
	name    string
	lines   []string // each with its newline, if it had one
	next    int      // index of the next line
	lno     int      // number of the line last read (as $line may set it)
	ifdepth int      // $if nesting when the file was opened
}

type pp struct {
	opts    Options
	defs    map[string]string
	files   []*infile // the stack of open files
	ifdepth int
	out     strings.Builder
	m       *Map
	errs    []*Error
}

// Preprocess preprocesses src, the contents of the file name.
func Preprocess(name, src string, opts Options) *Result {
	p := &pp{opts: opts, defs: map[string]string{}, m: &Map{}}
	for k, v := range opts.Predefined {
		p.defs[k] = v
	}
	p.push(name, src)
	for len(p.files) > 0 {
		f := p.cur()
		line, ok := p.readLine()
		if !ok {
			if f.ifdepth != p.ifdepth {
				p.fatal("unterminated $if", "")
				p.ifdepth = f.ifdepth
			}
			p.files = p.files[:len(p.files)-1]
			continue
		}
		p.line(f, line)
	}
	return &Result{Src: p.out.String(), Map: p.m, Errors: p.errs}
}

func (p *pp) cur() *infile { return p.files[len(p.files)-1] }

func (p *pp) push(name, src string) {
	f := &infile{name: name, ifdepth: p.ifdepth}
	for src != "" {
		i := strings.IndexByte(src, '\n')
		if i < 0 {
			f.lines = append(f.lines, src)
			break
		}
		f.lines = append(f.lines, src[:i+1])
		src = src[i+1:]
	}
	p.files = append(p.files, f)
}

// readLine reads the current file's next line, counting it.
func (p *pp) readLine() (string, bool) {
	f := p.cur()
	if f.next >= len(f.lines) {
		return "", false
	}
	s := f.lines[f.next]
	f.next++
	f.lno++
	return s, true
}

// emit writes one output line for source line lno of f.
func (p *pp) emit(f *infile, lno int, s string) {
	p.m.starts = append(p.m.starts, p.out.Len())
	p.m.pos = append(p.m.pos, Pos{f.name, lno})
	p.out.WriteString(s)
	if !strings.HasSuffix(s, "\n") {
		p.out.WriteByte('\n')
	}
}

func (p *pp) fatal(msg, value string) {
	f := p.cur()
	value = strings.TrimSuffix(value, "\n")
	if value != "" {
		msg = fmt.Sprintf("%q: %s", value, msg)
	}
	p.errs = append(p.errs, &Error{File: f.name, Line: f.lno, Msg: msg})
}

// line processes one line read from f.
func (p *pp) line(f *infile, s string) {
	lno := f.lno
	body := strings.TrimRight(s, "\n")
	t := strings.TrimLeft(body, " \t\v\f\r")
	switch {
	case isDirective(t):
		p.emit(f, lno, "\n")
		p.directive(t[1:], false)
	case strings.HasPrefix(body, "#line "):
		p.emit(f, lno, "\n")
		p.directive(body[1:], true)
	default:
		p.emit(f, lno, p.expand(s, nil))
	}
}

// isDirective reports whether t, a line without leading blanks, is a
// preprocessing directive: $ not followed by punctuation other than a
// space ($( and the other digraphs are Icon tokens).
func isDirective(t string) bool {
	if len(t) == 0 || t[0] != '$' {
		return false
	}
	return len(t) == 1 || !isPunct(t[1]) || t[1] == ' '
}

func isPunct(c byte) bool {
	return c > ' ' && c < 0x7f && !isAlnum(c)
}

func isAlpha(c byte) bool { return 'a' <= c && c <= 'z' || 'A' <= c && c <= 'Z' }
func isDigit(c byte) bool { return '0' <= c && c <= '9' }
func isAlnum(c byte) bool { return isAlpha(c) || isDigit(c) }
func isIdent(c byte) bool { return isAlnum(c) || c == '_' }
func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

// expand returns s with definitions substituted, as ipp's ppch scans: an
// identifier naming a definition not being expanded is replaced by its
// text, itself expanded; numbers, literals and comments are passed over.
func (p *pp) expand(s string, inuse map[string]bool) string {
	var b strings.Builder
	i := 0
	for i < len(s) {
		c := s[i]
		switch {
		case isAlpha(c) || c == '_':
			j := i + 1
			for j < len(s) && isIdent(s[j]) {
				j++
			}
			id := s[i:j]
			if v, ok := p.defs[id]; ok && !inuse[id] {
				in := map[string]bool{id: true}
				for k := range inuse {
					in[k] = true
				}
				b.WriteString(p.expand(v, in))
			} else {
				b.WriteString(id)
			}
			i = j
		case isDigit(c):
			j := nskip(s, i)
			b.WriteString(s[i:j])
			i = j
		case c == '#':
			b.WriteString(s[i:])
			i = len(s)
		case c == '"' || c == '\'':
			j := matchq(s, i)
			if j < len(s) {
				j++
			}
			b.WriteString(s[i:j])
			i = j
		default:
			b.WriteByte(c)
			i++
		}
	}
	return b.String()
}

// nskip returns the end of the number starting at s[i], as ipp's nskip.
func nskip(s string, i int) int {
	i++
	for i < len(s) && isDigit(s[i]) {
		i++
	}
	if i < len(s) && (s[i] == 'r' || s[i] == 'R') {
		i++
		for i < len(s) && isAlnum(s[i]) {
			i++
		}
		return i
	}
	if i < len(s) && s[i] == '.' {
		i++
		for i < len(s) && isDigit(s[i]) {
			i++
		}
	}
	if i < len(s) && (s[i] == 'e' || s[i] == 'E') {
		if i+1 < len(s) && (s[i+1] == '+' || s[i+1] == '-') {
			i++
		}
		i++
		for i < len(s) && isDigit(s[i]) {
			i++
		}
	}
	return i
}

// matchq returns the index of the quote closing the literal opened at
// s[i], or len(s) (or the newline) if there is none, as ipp's matchq.
func matchq(s string, i int) int {
	q := s[i]
	for i++; i < len(s) && s[i] != q && s[i] != '\n'; i++ {
		if s[i] == '\\' {
			i++
			if i >= len(s) {
				return len(s)
			}
		}
	}
	return i
}

// wskip skips blanks, and a comment to the end of the line.
func wskip(s string) string {
	s = strings.TrimLeft(s, " \t\v\f\r\n")
	if strings.HasPrefix(s, "#") {
		return ""
	}
	return s
}

// ident splits an identifier from the front of s.
func ident(s string) (string, string) {
	i := 0
	for i < len(s) && isIdent(s[i]) {
		i++
	}
	return s[:i], s[i:]
}

// filename splits a file name (an identifier, or a quoted string with
// backslash escapes) from the front of s; ok is false if a quoted name is
// unterminated.
func filename(s string) (name, rest string, ok bool) {
	if !strings.HasPrefix(s, "\"") {
		name, rest = ident(s)
		return name, rest, true
	}
	j := matchq(s, 0)
	if j >= len(s) || s[j] != '"' {
		return "", s[j:], false
	}
	var b strings.Builder
	for i := 1; i < j; i++ {
		if s[i] == '\\' && i+1 < j {
			i++
		}
		b.WriteByte(s[i])
	}
	return b.String(), s[j+1:], true
}

// directive processes the text after $ (or # for #line, whose errors
// are not reported).
func (p *pp) directive(s string, hashLine bool) {
	s = wskip(strings.TrimRight(s, "\n"))
	cmd, rest := ident(s)
	rest = wskip(rest)
	var msg string
	switch cmd {
	case "define":
		msg = p.define(rest)
	case "undef":
		msg = p.undef(rest)
	case "ifdef":
		msg = p.ifxdef(rest, true)
	case "ifndef":
		msg = p.ifxdef(rest, false)
	case "else":
		msg = p.elseDir(rest)
	case "endif":
		msg = p.endif(rest)
	case "include":
		msg = p.include(rest)
	case "line":
		msg = p.setLine(rest)
	case "error":
		p.fatal("explicit $error", rest)
	default:
		p.fatal("invalid preprocessing directive", cmd)
		return
	}
	if msg != "" && !(cmd == "line" && hashLine) {
		p.fatal(msg, "")
	}
}

func (p *pp) define(s string) string {
	if s == "" || !(isAlpha(s[0]) || s[0] == '_') {
		return "$define: missing name"
	}
	name, rest := ident(s)
	if strings.HasPrefix(rest, "(") {
		return "$define: \"(\" after name requires preceding space"
	}
	val := strings.TrimLeft(rest, " \t\v\f\r")
	if strings.HasPrefix(val, "#") {
		val = ""
	}
	// The value runs to a comment outside any literal.
	end := 0
	for end < len(val) && val[end] != '#' {
		if val[end] == '"' || val[end] == '\'' {
			j := matchq(val, end)
			if j >= len(val) || val[j] != val[end] {
				return "$define: unterminated literal"
			}
			end = j
		}
		end++
	}
	val = strings.TrimRight(val[:end], " \t\v\f\r\n")
	if old, ok := p.defs[name]; ok && old != val {
		p.fatal("value redefined", name)
		return ""
	}
	p.defs[name] = val
	return ""
}

func (p *pp) undef(s string) string {
	if s == "" || !(isAlpha(s[0]) || s[0] == '_') {
		return "$undef: missing name"
	}
	name, rest := ident(s)
	if wskip(rest) != "" {
		return "$undef: too many arguments"
	}
	delete(p.defs, name)
	return ""
}

func (p *pp) ifxdef(s string, want bool) string {
	p.ifdepth++
	if s == "" || !(isAlpha(s[0]) || s[0] == '_') {
		return "$ifdef/$ifndef: missing name"
	}
	name, rest := ident(s)
	if wskip(rest) != "" {
		return "$ifdef/$ifndef: too many arguments"
	}
	if _, ok := p.defs[name]; ok != want {
		p.skip(true)
	}
	return ""
}

func (p *pp) elseDir(s string) string {
	if p.ifdepth <= p.cur().ifdepth {
		return "unexpected $else"
	}
	if s != "" {
		p.fatal("extraneous arguments on $else/$endif", s)
	}
	p.skip(false)
	return ""
}

func (p *pp) endif(s string) string {
	if p.ifdepth <= p.cur().ifdepth {
		return "unexpected $endif"
	}
	if s != "" {
		p.fatal("extraneous arguments on $else/$endif", s)
	}
	p.ifdepth--
	return ""
}

// skip passes over lines to the $endif (or, if doElse, an $else) that
// ends the current section, as ipp's skipcode: nested $if sections are
// skipped whole, and $line is still obeyed. Skipped lines become empty.
func (p *pp) skip(doElse bool) {
	f := p.cur()
	for {
		s, ok := p.readLine()
		if !ok {
			return
		}
		p.emit(f, f.lno, "\n")
		body := strings.TrimRight(s, "\n")
		if strings.HasPrefix(body, "#line ") {
			p.setLine(wskip(body[len("#line"):]))
			continue
		}
		t := strings.TrimLeft(body, " \t\v\f\r")
		if !isDirective(t) {
			continue
		}
		cmd, rest := ident(wskip(t[1:]))
		rest = wskip(rest)
		switch {
		case strings.HasPrefix(cmd, "if"):
			p.ifdepth++
			p.skip(false)
		case cmd == "line":
			p.setLine(rest)
		case cmd == "endif" || (doElse && cmd == "else"):
			if rest != "" {
				p.fatal("extraneous arguments on $else/$endif", rest)
			}
			if cmd == "endif" {
				p.ifdepth--
			}
			return
		}
	}
}

func (p *pp) include(s string) string {
	name, rest, ok := filename(s)
	if !ok || name == "" {
		return "$include: invalid file name"
	}
	if wskip(rest) != "" {
		return "$include: too many arguments"
	}
	path, found := p.find(name)
	if found {
		for _, f := range p.files {
			if f.name == path {
				p.fatal("circular include", path)
				return ""
			}
		}
	}
	read := p.opts.ReadFile
	if read == nil {
		read = os.ReadFile
	}
	var b []byte
	var err error
	if found {
		b, err = read(path)
	}
	if !found || err != nil {
		p.fatal("cannot open", name)
		return ""
	}
	p.push(path, string(b))
	return ""
}

// find looks for an included file in the current directory, then in the
// include path, as icont's pathfind.
func (p *pp) find(name string) (string, bool) {
	exists := func(f string) bool {
		if p.opts.ReadFile != nil {
			_, err := p.opts.ReadFile(f)
			return err == nil
		}
		st, err := os.Stat(f)
		return err == nil && !st.IsDir()
	}
	if exists(name) {
		return name, true
	}
	if filepath.IsAbs(name) {
		return "", false
	}
	for _, dir := range p.opts.IncludePath {
		if f := filepath.Join(dir, name); exists(f) {
			return f, true
		}
	}
	return "", false
}

func (p *pp) setLine(s string) string {
	if s == "" || !isDigit(s[0]) {
		return "$line: no line number"
	}
	n := 0
	i := 0
	for i < len(s) && isDigit(s[i]) {
		n = 10*n + int(s[i]-'0')
		i++
	}
	s = wskip(s[i:])
	var name string
	if s != "" && (isAlpha(s[0]) || s[0] == '_' || s[0] == '"') {
		var ok bool
		name, s, ok = filename(s)
		if !ok || name == "" {
			return "$line: invalid file name"
		}
	}
	if wskip(s) != "" {
		return "$line: too many arguments"
	}
	f := p.cur()
	// As icont: the directive line itself becomes line n (its "#line n"
	// marker), so the next line read is n+1.
	f.lno = n
	if name != "" {
		f.name = name
	}
	return ""
}
