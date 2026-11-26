#!/bin/sh
# Build gcon-vm: bin/gcon-vm, from src/*.c. Needs only a C compiler
# ($CC, default cc). Never uses the network.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
# The callbacks of include/icall.h are exported from the executable for
# the libraries loadfunc loads (macOS exports an executable's symbols;
# ELF systems need -rdynamic, and older glibc -ldl for dlopen).
case "$(uname -s)" in
Linux*|GNU*) EXPORT="-rdynamic -ldl" ;;
*BSD*) EXPORT="-rdynamic" ;;
*) EXPORT= ;;
esac
mkdir -p bin
$CC -std=gnu11 -O2 -flto -Wall -Wextra ${CFLAGS:-} -o bin/gcon-vm src/*.c -lm $EXPORT
