package vm

import (
	"bufio"
	"errors"
	"io"
	"os"
	"os/exec"
	"sort"
	"strings"
	"unicode/utf8"
)

// File is an Icon file: an open file, a pipe, or a standard stream.
//
// Files are text unless opened with mode "u" (untranslated, in 9.5; for
// gcon, bytes): what is read is not validated or rewritten, only given
// the file's unit. See docs/design.md ("Strings and Unicode").
type File struct {
	name    string // the name opened, or the keyword for a standard file
	keyword bool   // a standard file: its image is its name
	r       *bufio.Reader
	w       *bufio.Writer
	os      *os.File  // for seek, where and close; nil for streams and pipes
	cmd     *exec.Cmd // for pipes
	closer  io.Closer // the pipe's end, for close
	unit    Unit
	closed  bool
	errout  bool // &errout: written unbuffered, after flushing &output
	// A directory opened for reading, as iconx's ReadDirectory: read and
	// !f produce its entries (with "." and ".."), reads(f, n) an entry
	// cut to n bytes; seek and where fail.
	isDir bool
	dir   []string // entries not yet read
}

func (f *File) image() string {
	if f.keyword {
		return f.name
	}
	return "file(" + f.name + ")"
}

func mustFile(v Value) *File {
	f, ok := v.Any().(*File)
	if !ok {
		Runerr(105, v)
	}
	return f
}

// fileArg returns a[i] as a file, defaulting to d.
func fileArg(a []Value, i int, d *File) *File {
	if _, null := arg(a, i).Any().(Null); null {
		return d
	}
	return mustFile(a[i])
}

func (vm *VM) reader(f *File) *bufio.Reader {
	if f.r == nil || f.closed {
		Runerr(212, V(f))
	}
	if f == vm.input {
		vm.output.w.Flush() // let a prompt appear before input is read
	}
	return f.r
}

func (vm *VM) writer(f *File) *bufio.Writer {
	if f.w == nil || f.closed {
		Runerr(213, V(f))
	}
	if f.errout {
		vm.output.w.Flush() // keep standard output and error in order
	}
	return f.w
}

// readLine reads a line; false at end of file. As in iconx, a line ends
// at "\n", "\r\n" or a lone "\r", except in an untranslated ("u") file,
// where only "\n" ends it and a "\r" is kept.
func (vm *VM) readLine(f *File) (*String, bool) {
	r := vm.reader(f)
	if f.isDir {
		return f.nextEntry(-1)
	}
	var b []byte
	for {
		c, err := r.ReadByte()
		if err != nil {
			if len(b) == 0 {
				return nil, false
			}
			break
		}
		if c == '\n' {
			break
		}
		if c == '\r' && f.unit == TextUnit {
			if next, err := r.Peek(1); err == nil && next[0] == '\n' {
				r.Discard(1)
			}
			break
		}
		b = append(b, c)
	}
	return NewString(string(b), f.unit), true
}

// readChars reads up to n characters; false at end of file. In text, a
// character is a UTF-8 sequence or an invalid byte, and is never split.
func (vm *VM) readChars(f *File, n int) (*String, bool) {
	r := vm.reader(f)
	if f.isDir {
		return f.nextEntry(n)
	}
	var b strings.Builder
	for k := 0; k < n; k++ {
		if f.unit == BytesUnit {
			c, err := r.ReadByte()
			if err != nil {
				break
			}
			b.WriteByte(c)
			continue
		}
		p, _ := r.Peek(utf8.UTFMax)
		if len(p) == 0 {
			break
		}
		_, w := utf8.DecodeRune(p)
		r.Discard(w)
		b.Write(p[:w])
	}
	if b.Len() == 0 {
		return nil, false
	}
	return NewString(b.String(), f.unit), true
}

// writeArgs implements write (nl) and writes: strings go to the current
// file, initially f; a file argument becomes the current file, and for
// write, after the first argument, ends the line on the file before it.
// It returns the last argument (&null if none).
//
// iconx flushes the file at the end of each call, so a call's output
// appears as a whole, and output from a call interrupted by an error
// appears only when the program exits, after the error message. gcon
// assembles each call's output before writing it, and keeps the output
// of an interrupted call for Run to write after the error message.
// Files the program opens are flushed at the end of each call, as in
// iconx; standard output stays buffered, and is flushed whenever
// anything else could observe it (writing to &errout, reading &input,
// running a command, exiting).
func (vm *VM) writeArgs(f *File, a []Value, nl bool) Value {
	var last Value = NullValue
	var buf []byte
	for n, v := range a {
		last = v
		switch x := v.Any().(type) {
		case *File:
			if nl && n > 0 {
				buf = append(buf, '\n')
				vm.emit(f, buf, true)
			} else if len(buf) > 0 {
				vm.emit(f, buf, false)
			}
			buf = nil
			f = x
		case Null:
		default:
			s, ok := CvStr(V(x))
			if !ok {
				vm.writer(f)
				if f.errout {
					vm.emit(f, buf, true)
				} else {
					vm.partial, vm.partialFile = buf, f
				}
				Runerr(109, V(x))
			}
			buf = append(buf, s.s...)
		}
	}
	if nl {
		buf = append(buf, '\n')
	}
	vm.emit(f, buf, true)
	return last
}

// emit writes the output of a write call to f; at the end of the call
// (end), files other than &output are flushed.
func (vm *VM) emit(f *File, b []byte, end bool) {
	w := vm.writer(f)
	if f.cmd != nil {
		vm.out.Flush() // the command may write to standard output at any time
	}
	w.Write(b)
	if end && f != vm.output {
		w.Flush()
	}
}

// openFile implements open(name, mode).
func (vm *VM) openFile(name, mode string) (*File, bool) {
	f := &File{name: name, unit: TextUnit}
	has := func(c byte) bool { return strings.IndexByte(mode, c) >= 0 }
	for i := 0; i < len(mode); i++ {
		if !strings.ContainsRune("rwabctupRWABCTUP", rune(mode[i])) {
			Runerr(209, V(Text(mode)))
		}
	}
	mode = strings.ToLower(mode)
	if has('u') {
		f.unit = BytesUnit
	}
	if has('p') {
		vm.output.w.Flush()
		cmd := vm.shell(name)
		vm.errout.Flush()
		if has('w') {
			cmd.Stdin = nil
			in, err := cmd.StdinPipe()
			if err != nil {
				return nil, false
			}
			f.w, f.closer = bufio.NewWriter(in), in
			vm.files = append(vm.files, f)
		} else {
			// The command reads gcon's standard input, as with popen,
			// but only when that is a file: a reader gcon was given
			// (as in tests) cannot be shared.
			cmd.Stdin = nil
			if vm.stdinFile != nil {
				cmd.Stdin = vm.stdinFile
			}
			cmd.Stdout = nil
			out, err := cmd.StdoutPipe()
			if err != nil {
				return nil, false
			}
			f.r = bufio.NewReader(out)
		}
		if cmd.Start() != nil {
			return nil, false
		}
		f.cmd = cmd
		return f, true
	}
	// As iconx: the letters set read, write, append and create, and these
	// choose C's fopen mode ("w" if create, else "a" if append, else "r"
	// if read, else "w"; with "+" for both read and write).
	read := has('r') || has('b')
	write := has('w') || has('b') || has('a') || has('c')
	if !read && !write {
		read = true
	}
	var flag int
	switch {
	case has('c'):
		flag = os.O_CREATE | os.O_TRUNC
	case has('a'):
		flag = os.O_CREATE | os.O_APPEND
	case read:
		flag = 0 // the file must exist
	default:
		flag = os.O_CREATE | os.O_TRUNC
	}
	switch {
	case read && write:
		flag |= os.O_RDWR
	case write:
		flag |= os.O_WRONLY
	default:
		flag |= os.O_RDONLY
	}
	osf, err := os.OpenFile(name, flag, 0o666)
	if err != nil {
		return nil, false
	}
	if st, err := osf.Stat(); err == nil && st.IsDir() && write {
		osf.Close()
		return nil, false
	}
	f.os = osf
	if st, err := osf.Stat(); err == nil && st.IsDir() {
		names, err := osf.Readdirnames(-1)
		osf.Close()
		if err != nil {
			return nil, false
		}
		sort.Strings(names)
		f.os, f.isDir = nil, true
		f.dir = append([]string{".", ".."}, names...)
		f.r = bufio.NewReader(strings.NewReader("")) // readable
		return f, true
	}
	if read {
		f.r = bufio.NewReader(osf)
	}
	if write {
		f.w = bufio.NewWriter(osf)
		vm.files = append(vm.files, f)
	}
	return f, true
}

// closeFile closes f, returning f, or for a pipe the command's exit
// code (iconx returns pclose's status shifted right 8 bits, masked to a
// byte: the exit code, not the wait status that system() returns).
func (vm *VM) closeFile(f *File) Value {
	if f.closed {
		return V(f)
	}
	if f.w != nil {
		f.w.Flush()
	}
	if f.keyword {
		return V(f)
	}
	if f.cmd != nil {
		vm.out.Flush() // the command may write to standard output as it ends
	}
	f.closed = true
	if f.closer != nil {
		f.closer.Close()
	}
	if f.cmd != nil {
		return V(Int(waitStatus(f.cmd.Wait()) >> 8 & 0xff))
	}
	if f.os != nil {
		f.os.Close()
	}
	return V(f)
}

// shell returns a command that runs s with /bin/sh as C's system and
// popen do (so sh's messages begin "sh:"), with gcon's standard streams,
// as files when they are.
func (vm *VM) shell(s string) *exec.Cmd {
	cmd := exec.Command("/bin/sh", "-c", s)
	cmd.Args[0] = "sh"
	cmd.Stdin, cmd.Stdout, cmd.Stderr = vm.stdin, vm.stdout, vm.stderr
	if vm.stdinFile != nil {
		cmd.Stdin = vm.stdinFile
	}
	if vm.stdoutFile != nil {
		cmd.Stdout = vm.stdoutFile
	}
	if vm.stderrFile != nil {
		cmd.Stderr = vm.stderrFile
	}
	return cmd
}

// waitStatus returns the C wait status of a finished command, as iconx's
// system() and close() of a pipe return it: the exit code times 256.
func waitStatus(err error) int {
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		return ee.ExitCode() << 8
	}
	if err != nil {
		return 127 << 8
	}
	return 0
}

// lineGen generates the lines of a file (!f).
type lineGen struct{ f *File }

func (g lineGen) Next(vm *VM) (Value, bool) {
	s, ok := vm.readLine(g.f)
	return V(s), ok
}

func init() {
	Ops["&input"] = &Builtin{Name: "&input", Fn: func(vm *VM, a []Value) (Value, bool) { return V(vm.input), true }}
	Ops["&output"] = &Builtin{Name: "&output", Fn: func(vm *VM, a []Value) (Value, bool) { return V(vm.output), true }}
	Ops["&errout"] = &Builtin{Name: "&errout", Fn: func(vm *VM, a []Value) (Value, bool) { return V(vm.errfile), true }}

	fn("write", func(vm *VM, a []Value) (Value, bool) { return vm.writeArgs(vm.output, a, true), true })
	fn("writes", func(vm *VM, a []Value) (Value, bool) { return vm.writeArgs(vm.output, a, false), true })
	fn("stop", func(vm *VM, a []Value) (Value, bool) {
		vm.writeArgs(vm.errfile, a, true)
		vm.Exit(1)
		return Value{}, false
	})
	fn("open", func(vm *VM, a []Value) (Value, bool) {
		mode := "r"
		if _, null := arg(a, 1).Any().(Null); !null {
			mode = MustStr(a[1]).s
		}
		f, ok := vm.openFile(MustStr(arg(a, 0)).s, mode)
		return V(f), ok
	})
	fn("close", func(vm *VM, a []Value) (Value, bool) {
		return vm.closeFile(mustFile(arg(a, 0))), true
	})
	fn("flush", func(vm *VM, a []Value) (Value, bool) {
		f := mustFile(arg(a, 0))
		if f.w != nil {
			f.w.Flush()
		}
		return V(f), true
	})
	fn("read", func(vm *VM, a []Value) (Value, bool) {
		s, ok := vm.readLine(fileArg(a, 0, vm.input))
		return V(s), ok
	})
	fn("reads", func(vm *VM, a []Value) (Value, bool) {
		f := fileArg(a, 0, vm.input)
		n := 1
		if _, null := arg(a, 1).Any().(Null); !null {
			n = int(MustInt(a[1]))
		}
		if n <= 0 {
			Runerr(205, V(Int(n)))
		}
		s, ok := vm.readChars(f, n)
		return V(s), ok
	})
	fn("seek", func(vm *VM, a []Value) (Value, bool) {
		f := mustFile(arg(a, 0))
		i := MustInt(arg(a, 1))
		if f.os == nil {
			return Value{}, false
		}
		if f.w != nil {
			f.w.Flush()
		}
		var err error
		if i > 0 {
			_, err = f.os.Seek(int64(i-1), io.SeekStart)
		} else {
			_, err = f.os.Seek(int64(i), io.SeekEnd)
		}
		if err != nil {
			return Value{}, false
		}
		if f.r != nil {
			f.r.Reset(f.os)
		}
		return V(f), true
	})
	fn("where", func(vm *VM, a []Value) (Value, bool) {
		f := mustFile(arg(a, 0))
		if f.os == nil {
			return Value{}, false
		}
		if f.w != nil {
			f.w.Flush()
		}
		off, err := f.os.Seek(0, io.SeekCurrent)
		if err != nil {
			return Value{}, false
		}
		if f.r != nil {
			off -= int64(f.r.Buffered())
		}
		return V(Int(off + 1)), true
	})
	fn("remove", func(vm *VM, a []Value) (Value, bool) {
		return NullValue, os.Remove(MustStr(arg(a, 0)).s) == nil
	})
	fn("rename", func(vm *VM, a []Value) (Value, bool) {
		return NullValue, os.Rename(MustStr(arg(a, 0)).s, MustStr(arg(a, 1)).s) == nil
	})
	fn("getenv", func(vm *VM, a []Value) (Value, bool) {
		v, ok := os.LookupEnv(MustStr(arg(a, 0)).s)
		return V(Text(v)), ok
	})
	fn("system", func(vm *VM, a []Value) (Value, bool) {
		vm.output.w.Flush()
		vm.errfile.w.Flush()
		cmd := vm.shell(MustStr(arg(a, 0)).s)
		return V(Int(waitStatus(cmd.Run()))), true
	})
}

// nextEntry returns the next entry of a directory, cut to n bytes if n
// is not negative, or fails at the end.
func (f *File) nextEntry(n int) (*String, bool) {
	if len(f.dir) == 0 {
		return nil, false
	}
	e := f.dir[0]
	f.dir = f.dir[1:]
	if n >= 0 && len(e) > n {
		e = e[:n]
	}
	return NewString(e, f.unit), true
}
