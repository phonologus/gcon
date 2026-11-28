#!/bin/sh
# The Cocoa display server's tests: graphics.sh, ref.sh and window.sh,
# run with gcon-graphics-cocoa (../graphics/cocoa; built here unless
# $GCONGRAPHICS names one). window.sh opens real windows, briefly. The
# Cocoa server draws in copy mode with Core Graphics (aliased in ref.sh,
# which turns gcon's antialias attribute off), so ref.sh allows the edges
# of slanted lines and curves to differ from
# Arizona Icon's X11 reference, and text (tests/ref/tolerance-cocoa);
# graphics.sh's output is the SDL server's (the same fonts, through
# CoreText).
#
#   tests/cocoa.sh
#
# Off macOS it says so and does nothing.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$here/.." && pwd)

if [ -z "${GCONGRAPHICS:-}" ]; then
	if ! sh "$src/graphics/cocoa/build.sh" -check; then
		echo "cocoa: not macOS: the Cocoa server is not checked"
		exit 0
	fi
	sh "$src/graphics/cocoa/build.sh" >/dev/null || exit 2
	GCONGRAPHICS=$src/graphics/bin/gcon-graphics-cocoa
fi
export GCONGRAPHICS

fail=0
sh "$here/tests/graphics.sh" || fail=1
sh "$here/tests/ref.sh" || fail=1
sh "$here/tests/window.sh" || fail=1
[ $fail = 0 ] && echo "cocoa: pass" || echo "cocoa: FAIL"
exit $fail
