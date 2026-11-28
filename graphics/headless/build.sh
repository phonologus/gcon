#!/bin/sh
# Build gcon-graphics-headless, the display server with no display:
# ../src/main.c (the protocol reader) and ../src/xcolors.c, with gcon's
# own rasterizer (../raster) and this directory's layers. Hidden
# canvases only. Its fonts are X's misc-fixed (no lookup of the
# platform's), from ../../deps/fixed
# (checked against ../../deps/deps.lock), compiled in as tables that
# ../raster/bdf2c.sh writes. Needs a C compiler alone. The binary is left
# in ../bin/gcon-graphics-headless. $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
deps=../../deps
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'; }
grep -v '^#' "$deps/deps.lock" | awk 'index($1, "fixed/") == 1' | while read -r name ver sha url; do
	[ "$(sha256 "$deps/$name")" = "$sha" ] ||
		{ echo "headless/build.sh: deps/$name does not match deps/deps.lock" >&2; exit 1; }
done
sh ../raster/bdf2c.sh "$deps"/fixed/*.bdf >"$tmp/fixedfonts.inc"
mkdir -p ../bin
# shellcheck disable=SC2086 # the flags are a list
$CC -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -I../src -I../raster -I"$tmp" \
	-o ../bin/gcon-graphics-headless.new ../src/main.c ../src/xcolors.c ../raster/raster.c \
	../raster/rcanvas.c ../raster/rfont.c hplat.c -lm
mv ../bin/gcon-graphics-headless.new ../bin/gcon-graphics-headless
echo "headless/build.sh: built bin/gcon-graphics-headless"
