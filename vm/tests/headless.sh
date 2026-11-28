#!/bin/sh
# The headless display server's tests: graphics.sh and ref.sh, run with
# gcon-graphics-headless (../graphics/headless; built here unless
# $GCONGRAPHICS names one), which needs no display (it has no windows,
# so window.sh does not apply). It draws with gcon's own rasterizer and
# X's misc-fixed fonts: ref.sh allows a few pixels at arc edges and any
# text (tests/ref/tolerance-headless), and graphics.sh's output is
# tests/graphics-headless.std (the X11 server's but for the fonts).
#
#   tests/headless.sh
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$here/.." && pwd)
if [ -z "${GCONGRAPHICS:-}" ]; then
	sh "$src/graphics/headless/build.sh" >/dev/null || exit 2
	GCONGRAPHICS=$src/graphics/bin/gcon-graphics-headless
fi
export GCONGRAPHICS
fail=0
sh "$here/tests/graphics.sh" || fail=1
sh "$here/tests/ref.sh" || fail=1
[ $fail = 0 ] && echo "headless: pass" || echo "headless: FAIL"
exit $fail
