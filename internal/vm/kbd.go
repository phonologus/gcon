package vm

import (
	"io"
	"os"
	"unicode/utf8"
)

// The keyboard functions getch, getche and kbhit, as iconx's on Unix
// (rsys.r): while one runs, a terminal on standard input is put in
// non-canonical mode (characters are available as typed, without
// waiting for a newline), with or without echo, and restored after.
// They read through &input's buffer, so a character kbhit sees is still
// there for getch or read. When standard input is not a terminal they
// simply read from it.

// withTerminal runs f with standard input's terminal modes changed by
// set, if standard input is a terminal, restoring them afterwards, and
// reports whether it was a terminal.
func (vm *VM) withTerminal(set func(t *termios), f func()) bool {
	in, ok := vm.stdin.(*os.File)
	if !ok {
		f()
		return false
	}
	old, err := getTermios(in.Fd())
	if err != nil {
		f()
		return false
	}
	t := *old
	set(&t)
	setTermios(in.Fd(), &t)
	defer setTermios(in.Fd(), old)
	f()
	return true
}

// getKey reads one character (a byte) from the keyboard.
func (vm *VM) getKey(echo bool) (Value, bool) {
	r := vm.reader(vm.input)
	var c byte
	var err error
	vm.withTerminal(func(t *termios) { rawMode(t, echo, true) }, func() {
		c, err = r.ReadByte()
	})
	if err != nil {
		return Value{}, false
	}
	if c < utf8.RuneSelf {
		return StrVal(Text(string(rune(c)))), true
	}
	return StrVal(NewString(string([]byte{c}), BytesUnit)), true
}

func init() {
	fn("getch", func(vm *VM, a []Value) (Value, bool) { return vm.getKey(false) })
	fn("getche", func(vm *VM, a []Value) (Value, bool) { return vm.getKey(true) })
	fn("kbhit", func(vm *VM, a []Value) (Value, bool) {
		r := vm.reader(vm.input)
		if r.Buffered() > 0 {
			return NullValue, true
		}
		var err error
		tty := vm.withTerminal(func(t *termios) { rawMode(t, true, false) }, func() {
			_, err = r.Peek(1)
		})
		// On a terminal, nothing typed reads as end of file; otherwise
		// end of file counts as ready, as select reports it in iconx.
		return NullValue, err == nil || (!tty && err == io.EOF)
	})
}
