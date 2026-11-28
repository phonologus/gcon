#!/bin/sh
# Build gcon-vm: bin/gcon-vm, from src/*.c (and ../deps/stb's headers,
# checked against ../deps/deps.lock). Needs only a C compiler
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
# gcc: -flto=auto links in parallel (a plain -flto makes gcc warn that it
# links serially); and, as gcc's manual advises for computed gotos,
# -fno-gcse -fno-crossjumping keep each opcode's own dispatch jump, which
# gcc otherwise merges into one (2% on phwbi, Fedora x86_64). clang: -flto.
LTO=-flto
if $CC --version 2>/dev/null | grep -qi 'gcc\|free software foundation' && ! $CC --version 2>/dev/null | grep -qi clang; then
	LTO="-flto=auto -fno-gcse -fno-crossjumping"
fi
# The image codecs (src/imgcodec.c) include stb, vendored in ../deps/stb
# and checked against ../deps/deps.lock.
sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'; }
grep -v '^#' ../deps/deps.lock | awk 'index($1, "stb/") == 1' | while read -r name ver sha url; do
	[ "$(sha256 "../deps/$name")" = "$sha" ] ||
		{ echo "vm/build.sh: deps/$name does not match deps/deps.lock" >&2; exit 1; }
done
mkdir -p bin
# -ffp-contract=off: no fused multiply-adds, which GCC makes by default
# under gnu11 on arm64, so that reals (and so where curves and arcs fall)
# are the same whatever the compiler and machine
$CC -std=gnu11 -O2 -ffp-contract=off $LTO -Wall -Wextra ${CFLAGS:-} -I../deps/stb -o bin/gcon-vm src/*.c -lm $EXPORT
