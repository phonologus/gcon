#!/bin/sh
# Build gcon-graphics-x11, the X11 display server: ../src/main.c (the
# protocol reader) and ../src/xcolors.c, with this directory's Xlib
# layers. Needs a C compiler and Xlib (libX11 and its headers), found by
# pkg-config if there is one, else in /opt/X11 (XQuartz on macOS),
# /usr/X11R6, /usr/X11 or the compiler's own paths. The binary is left
# in ../bin/gcon-graphics-x11.
#
#   x11/build.sh          build
#   x11/build.sh -check   only say whether Xlib is found (exit status 0 if so)
#
# $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# xflags: the compiler and linker flags for Xlib, or fail
printf '#include <X11/Xlib.h>\nint main(void) { return XOpenDisplay(0) == 0 ? 0 : 0; }\n' >"$tmp/probe.c"
xlib=
try() {
	$CC $1 -o "$tmp/probe" "$tmp/probe.c" $2 >/dev/null 2>&1 && xlib="$1|$2"
}
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x11 2>/dev/null; then
	try "$(pkg-config --cflags x11)" "$(pkg-config --libs x11)"
fi
for d in /opt/X11 /usr/X11R6 /usr/X11; do
	[ -z "$xlib" ] && [ -f "$d/include/X11/Xlib.h" ] && try "-I$d/include" "-L$d/lib -lX11"
done
[ -z "$xlib" ] && try "" "-lX11"
if [ "${1:-}" = -check ]; then
	[ -n "$xlib" ]
	exit
fi
[ -n "$xlib" ] || { echo "x11/build.sh: no Xlib (libX11 and its headers) found" >&2; exit 1; }
cflags=${xlib%%|*}
libs=${xlib#*|}
mkdir -p ../bin
# shellcheck disable=SC2086 # the flags are lists
$CC -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -I../src $cflags \
	-o ../bin/gcon-graphics-x11.new ../src/main.c ../src/xcolors.c xplat.c xcanvas.c xfont.c $libs -lm
mv ../bin/gcon-graphics-x11.new ../bin/gcon-graphics-x11
echo "x11/build.sh: built bin/gcon-graphics-x11"
