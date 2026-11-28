#!/bin/sh
# The headless display server's tests: graphics.sh and ref.sh, run with
# gcon-graphics-headless (../graphics/headless; built here unless
# $GCONGRAPHICS names one), which needs no display (it has no windows,
# so window.sh does not apply). It draws with gcon's own rasterizer and
# X's misc-fixed fonts: ref.sh allows a few pixels at arc edges and any
# text (tests/ref/tolerance-headless), and graphics.sh's output is
# tests/graphics-headless.std (the X11 server's but for the fonts). And the
# fallback: gcon-vm runs it where the default server has no display, or
# there is none.
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

# The fallback: gcon-vm with no default server beside it, or one with no
# display (the X11 server without DISPLAY, if it is built), runs the
# headless server: hidden canvases work, and a window cannot be opened.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/root"
cp "${VM:-$here/bin/gcon-vm}" "$tmp/root/gcon-vm"
cp "$GCONGRAPHICS" "$tmp/root/gcon-graphics-headless"
cat >"$tmp/fb.icn" <<'ICN'
procedure main()
   p := open("p", "g", "canvas=hidden", "size=20,10", "bg=white") | stop("no hidden canvas")
   Fg(p, "red"); FillRectangle(p, 0, 0, 10, 10)
   write("hidden canvas: ", Pixel(p, 5, 5, 1, 1), " ", Pixel(p, 15, 5, 1, 1))
   write("WriteImage: ", (WriteImage(p, "fb.png") & "written") | "failed")
   write("window: ", image(open("w", "g")) | "open fails")
end
ICN
printf 'hidden canvas: 65535,0,0 65535,65535,65535\nWriteImage: written\nwindow: open fails\n' >"$tmp/want"
GCON=${GCON:-}
if [ -z "$GCON" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c"
	GCON=$tmp/gcon
fi
(cd "$tmp" && GCONROOT=$src "$GCON" build -o fb fb.icn)
fallback() { # NAME: run fb with gcon-vm in the scratch root, no display
	(cd "$tmp" && env -u DISPLAY -u GCONGRAPHICS GCONVM="$tmp/root/gcon-vm" ./fb >got 2>&1) || true
	cmp -s "$tmp/want" "$tmp/got" && echo "fallback ($1): pass" ||
		{ echo "FAIL fallback ($1): $(cat "$tmp/got")"; fail=1; }
}
fallback "no default server"
if [ -x "$src/graphics/bin/gcon-graphics-x11" ]; then
	cp "$src/graphics/bin/gcon-graphics-x11" "$tmp/root/gcon-graphics"
	fallback "the X11 server, no DISPLAY"
fi
[ $fail = 0 ] && echo "headless: pass" || echo "headless: FAIL"
exit $fail
