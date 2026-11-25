//go:build darwin || linux

package vm

import (
	"syscall"
	"unsafe"
)

type termios = syscall.Termios

func getTermios(fd uintptr) (*termios, error) {
	var t termios
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd, ioctlGet, uintptr(unsafe.Pointer(&t))); e != 0 {
		return nil, e
	}
	return &t, nil
}

func setTermios(fd uintptr, t *termios) {
	syscall.Syscall(syscall.SYS_IOCTL, fd, ioctlSet, uintptr(unsafe.Pointer(t)))
}

// rawMode makes t non-canonical, with echo as given; a blocking read
// waits for one character, a non-blocking one returns at once.
func rawMode(t *termios, echo, block bool) {
	t.Lflag &^= syscall.ICANON
	if echo {
		t.Lflag |= syscall.ECHO
	} else {
		t.Lflag &^= syscall.ECHO
	}
	if block {
		t.Cc[syscall.VMIN], t.Cc[syscall.VTIME] = 1, 0
	} else {
		t.Cc[syscall.VMIN], t.Cc[syscall.VTIME] = 0, 0
	}
}
