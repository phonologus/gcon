//go:build !darwin && !linux

package vm

import "errors"

// termios is a placeholder: without terminal control, the keyboard
// functions read standard input as it comes.
type termios struct{}

func getTermios(fd uintptr) (*termios, error) { return nil, errors.New("no terminal control") }
func setTermios(fd uintptr, t *termios)       {}
func rawMode(t *termios, echo, block bool)    {}
