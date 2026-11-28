#!/bin/sh
# Build gcon-graphics-cocoa, the macOS display server: ../src/main.c (the
# protocol reader) and ../src/xcolors.c, with this directory's AppKit,
# Core Graphics, CoreText and ImageIO layers. Needs macOS and its C
# compiler (clang, which compiles the Objective-C); the frameworks are
# the system's. The binary is left in ../bin/gcon-graphics-cocoa.
#
#   cocoa/build.sh          build
#   cocoa/build.sh -check   only say whether it can be built here (exit status 0 if so)
#
# $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
if [ "${1:-}" = -check ]; then
	[ "$(uname)" = Darwin ]
	exit
fi
[ "$(uname)" = Darwin ] || { echo "cocoa/build.sh: the Cocoa server is for macOS" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags="-O2 -Wall -Wextra ${CFLAGS:-} -I../src"
# shellcheck disable=SC2086 # the flags are lists
for f in ../src/main.c ../src/xcolors.c ccanvas.c cfont.c; do
	$CC -std=c11 $flags -c -o "$tmp/$(basename "$f" .c).o" "$f"
done
# shellcheck disable=SC2086
$CC $flags -fobjc-arc -c -o "$tmp/cplat.o" cplat.m
mkdir -p ../bin
$CC -o ../bin/gcon-graphics-cocoa.new "$tmp"/*.o -framework AppKit -framework QuartzCore \
	-framework CoreGraphics -framework CoreText -framework ImageIO -lm
mv ../bin/gcon-graphics-cocoa.new ../bin/gcon-graphics-cocoa
echo "cocoa/build.sh: built bin/gcon-graphics-cocoa"
