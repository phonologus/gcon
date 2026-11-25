//go:build !unix

package run

import "errors"

const canExec = false

func execve(path string, argv []string) error { return errors.New("not supported") }

func tempProgram(enc []byte) (int, error) { return 0, errors.New("not supported") }
