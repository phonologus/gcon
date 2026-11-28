#!/bin/sh
# The X11 display server's tests: graphics.sh, ref.sh and window.sh,
# run with gcon-graphics-x11 (../graphics/x11; built here unless
# $GCONGRAPHICS names one) on a display of their own, an Xvfb started
# for them (XQuartz's on macOS, /opt/X11/bin/Xvfb), else $DISPLAY. The
# X11 server draws with X's own requests, so ref.sh's images must match
# Arizona Icon's X11 reference exactly but for text
# (tests/ref/tolerance-x11), and graphics.sh's output is the SDL
# server's but for the fonts (tests/graphics-x11.std, made with
# XQuartz's core fonts: another X server's fonts may differ).
#
#   tests/x11.sh
#
# Without Xlib, or without Xvfb and $DISPLAY, it says so and does
# nothing.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
xvfb=
trap '[ -n "$xvfb" ] && kill $xvfb 2>/dev/null; rm -rf "$tmp"' EXIT

if [ -z "${GCONGRAPHICS:-}" ]; then
	if ! sh "$src/graphics/x11/build.sh" -check; then
		echo "x11: no Xlib: the X11 server is not checked"
		exit 0
	fi
	sh "$src/graphics/x11/build.sh" >/dev/null || exit 2
	GCONGRAPHICS=$src/graphics/bin/gcon-graphics-x11
fi
export GCONGRAPHICS

# a display: a private Xvfb on the first free number from :90
X=$(command -v Xvfb || true)
[ -z "$X" ] && [ -x /opt/X11/bin/Xvfb ] && X=/opt/X11/bin/Xvfb
if [ -n "$X" ]; then
	n=90
	while [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ]; do
		n=$((n + 1))
	done
	"$X" ":$n" -screen 0 1280x1024x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
	xvfb=$!
	DISPLAY=:$n
	i=0
	while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $i -lt 50 ] && kill -0 $xvfb 2>/dev/null; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -e "/tmp/.X11-unix/X$n" ] || { echo "x11: Xvfb did not start: $(cat "$tmp/xvfb.log")"; exit 2; }
elif [ -z "${DISPLAY:-}" ]; then
	echo "x11: no Xvfb and no DISPLAY: the X11 server is not checked"
	exit 0
fi
export DISPLAY

fail=0
GRAPHICSSTD=$here/tests/graphics-x11.std sh "$here/tests/graphics.sh" || fail=1
REFTOLERANCE=$here/tests/ref/tolerance-x11 sh "$here/tests/ref.sh" || fail=1
sh "$here/tests/window.sh" || fail=1
[ $fail = 0 ] && echo "x11: pass" || echo "x11: FAIL"
exit $fail
