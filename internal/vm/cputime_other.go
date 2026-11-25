//go:build !darwin && !linux

package vm

import "time"

var started = time.Now()

// cpuMillis approximates processor time by elapsed time.
func cpuMillis() int64 { return time.Since(started).Milliseconds() }
