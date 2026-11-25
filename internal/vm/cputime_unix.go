//go:build darwin || linux

package vm

import "syscall"

// cpuMillis is the program's processor time in milliseconds, as iconx's
// millisec(): user and system time.
func cpuMillis() int64 {
	var ru syscall.Rusage
	if syscall.Getrusage(syscall.RUSAGE_SELF, &ru) != nil {
		return 0
	}
	ms := func(t syscall.Timeval) int64 { return int64(t.Sec)*1000 + int64(t.Usec)/1000 }
	return ms(ru.Utime) + ms(ru.Stime)
}
