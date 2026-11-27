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
# Link-time optimization: gcc takes -flto=auto, which links in parallel
# (a plain -flto makes it warn that it links serially); clang -flto.
LTO=-flto
if $CC --version 2>/dev/null | grep -qi 'gcc\|free software foundation' && ! $CC --version 2>/dev/null | grep -qi clang; then
	LTO=-flto=auto
fi
mkdir -p bin
$CC -std=gnu11 -O2 $LTO -Wall -Wextra ${CFLAGS:-} -o bin/gcon-vm src/*.c -lm $EXPORT
