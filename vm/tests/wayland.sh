#!/bin/sh
# The Wayland display server's tests: graphics.sh, ref.sh and window.sh,
# run with gcon-graphics-wayland (../graphics/wayland; built here unless
# $GCONGRAPHICS names one) on a compositor of their own, Weston's
# headless backend, else $WAYLAND_DISPLAY. Its drawing is gcon's
# rasterizer, so ref.sh holds it to the headless server's tolerances
# (tests/ref/tolerance-headless), and its fonts are fontconfig's
# (tests/graphics-fontconfig.std). window.sh runs twice: with libdecor's
# title bars (by its cairo plugin, where installed: GTK's complains on
# stderr of a compositor without a seat), and with GCONLIBDECOR=off.
#
#   tests/wayland.sh
#
# Without the Wayland libraries, or without Weston and $WAYLAND_DISPLAY,
# it says so and does nothing.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
weston=
trap '[ -n "$weston" ] && kill $weston 2>/dev/null; rm -rf "$tmp"' EXIT

if [ -z "${GCONGRAPHICS:-}" ]; then
	if ! sh "$src/graphics/wayland/build.sh" -check; then
		echo "wayland: no Wayland libraries: the Wayland server is not checked"
		exit 0
	fi
	sh "$src/graphics/wayland/build.sh" >/dev/null || exit 2
	GCONGRAPHICS=$src/graphics/bin/gcon-graphics-wayland
fi
export GCONGRAPHICS

# a compositor: a private headless Weston, its socket in a runtime
# directory of its own
if command -v weston >/dev/null 2>&1; then
	export XDG_RUNTIME_DIR=$tmp/run
	mkdir -m 700 "$XDG_RUNTIME_DIR"
	weston --backend=headless --socket=gcon-test --width=1280 --height=1024 \
		--no-config >"$tmp/weston.log" 2>&1 &
	weston=$!
	i=0
	while [ ! -e "$XDG_RUNTIME_DIR/gcon-test" ] && [ $i -lt 50 ] && kill -0 $weston 2>/dev/null; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -e "$XDG_RUNTIME_DIR/gcon-test" ] || { echo "wayland: Weston did not start: $(cat "$tmp/weston.log")"; exit 2; }
	WAYLAND_DISPLAY=gcon-test
elif [ -z "${WAYLAND_DISPLAY:-}" ]; then
	echo "wayland: no Weston and no WAYLAND_DISPLAY: the Wayland server is not checked"
	exit 0
fi
export WAYLAND_DISPLAY
unset DISPLAY

# libdecor's cairo plugin alone, where it is installed
for d in $(pkg-config --variable=libdir libdecor-0 2>/dev/null) /usr/lib/*/ /usr/lib64 /usr/lib; do
	if [ -f "$d/libdecor/plugins-1/libdecor-cairo.so" ]; then
		mkdir "$tmp/plugins"
		ln -s "$d/libdecor/plugins-1/libdecor-cairo.so" "$tmp/plugins/"
		export LIBDECOR_PLUGIN_DIR=$tmp/plugins
		break
	fi
done

fail=0
sh "$here/tests/graphics.sh" || fail=1
sh "$here/tests/ref.sh" || fail=1
sh "$here/tests/window.sh" || fail=1
GCONLIBDECOR=off sh "$here/tests/window.sh" || fail=1
[ $fail = 0 ] && echo "wayland: pass" || echo "wayland: FAIL"
exit $fail
