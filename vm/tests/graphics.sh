#!/bin/sh
# Runs tests/graphics.icn (graphics on a hidden canvas, so no display is
# needed) and compares its output with tests/graphics.std. Needs gcon
# ($GCON, or built from $GCONSRC, default this checkout) and a display
# server: $GCONGRAPHICS, by default the one build.sh installed
# (../libexec/gcon-graphics); without one it says so and checks nothing.
# The X11 server's fonts are X's, so its output is tests/graphics-x11.std,
# and the headless server's misc-fixed, tests/graphics-headless.std;
# GRAPHICSSTD names other expected output.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd)
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c"
	GCON=$tmp/gcon
fi
export GCONGRAPHICS=${GCONGRAPHICS:-$here/../libexec/gcon-graphics}
unset GCONANTIALIAS # the default (on), whatever the user's
if [ ! -x "$GCONGRAPHICS" ]; then
	echo "graphics: no display server ($GCONGRAPHICS): not checked"
	exit 0
fi
# the X11 server's fonts are X's
case $("$GCONGRAPHICS" -version 2>/dev/null) in
*X11*) GRAPHICSSTD=${GRAPHICSSTD:-$here/tests/graphics-x11.std} ;;
*headless*) GRAPHICSSTD=${GRAPHICSSTD:-$here/tests/graphics-headless.std} ;;
esac
cp "$here/tests/graphics.icn" "$tmp"
(cd "$tmp" && "$GCON" build -o graphics graphics.icn)
(cd "$tmp" && "$vm" ./graphics >out 2>&1) || true
if diff "${GRAPHICSSTD:-$here/tests/graphics.std}" "$tmp/out"; then
	echo "graphics: pass"
else
	echo "graphics: FAIL"
	exit 1
fi
