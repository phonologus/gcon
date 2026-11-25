//go:build unix

package run

import (
	"os"
	"syscall"
)

const canExec = true

func execve(path string, argv []string) error { return syscall.Exec(path, argv, os.Environ()) }

// tempProgram writes enc to a file that is deleted at once, returning a
// descriptor of it that an exec'd process inherits.
func tempProgram(enc []byte) (int, error) {
	f, err := os.CreateTemp("", "gcon-*.prog")
	if err != nil {
		return 0, err
	}
	os.Remove(f.Name())
	if _, err := f.Write(enc); err != nil {
		f.Close()
		return 0, err
	}
	// Opening /dev/fd/N shares the offset on macOS: rewind.
	if _, err := f.Seek(0, 0); err != nil {
		f.Close()
		return 0, err
	}
	// Go opens files close-on-exec; a dup is not.
	fd, err := syscall.Dup(int(f.Fd()))
	f.Close()
	return fd, err
}
