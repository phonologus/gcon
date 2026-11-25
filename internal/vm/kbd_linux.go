package vm

import "syscall"

const ioctlGet, ioctlSet = syscall.TCGETS, syscall.TCSETS
