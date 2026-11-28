#!/bin/sh
# Build gcon-graphics-headless, the display server with no display:
# ../src/main.c (the protocol reader) and ../src/xcolors.c, with
# gcon's own rasterizer (../raster) and this directory's layers. Hidden
# canvases only. Needs a C compiler alone. The binary is left in
# ../bin/gcon-graphics-headless. $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
mkdir -p ../bin
# shellcheck disable=SC2086 # the flags are a list
$CC -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -I../src -I../raster \
	-o ../bin/gcon-graphics-headless.new ../src/main.c ../src/xcolors.c ../raster/raster.c hcanvas.c hplat.c -lm
mv ../bin/gcon-graphics-headless.new ../bin/gcon-graphics-headless
echo "headless/build.sh: built bin/gcon-graphics-headless"
