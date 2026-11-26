#!/bin/sh
# Window tests: these open real windows (briefly), so they need a
# display, and the display server (../graphics/bin/gcon-graphics, or
# $GCONGRAPHICS). Each window/NAME.icn runs with ICONSDL_TEST_EVENTS
# set from NAME.script (the server pushes those keystrokes and clicks
# into the window as it opens); its standard output and error must
# match NAME.std. NAME.env, if present, holds VAR=value lines added to
# its environment. Most come from icon-sdl's sdl/tests/win, whose
# expected output is iconx's.
#
#   tests/window.sh [name ...]
#
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
GCONROOT=${GCONROOT:-$src}
export GCONROOT
export GCONGRAPHICS=${GCONGRAPHICS:-$src/graphics/bin/gcon-graphics}
export GCONVM=${GCONVM:-$here/bin/gcon-vm}
if [ $# -eq 0 ]; then
	set -- $(cd "$here/tests/window" && ls *.icn | sed 's/\.icn$//')
fi
pass=0 fail=0
cd "$tmp" || exit 2
for n; do
	d=$here/tests/window
	cp "$d/$n.icn" .
	script=""
	[ -f "$d/$n.script" ] && script=$(cat "$d/$n.script")
	envs=""
	[ -f "$d/$n.env" ] && envs=$(cat "$d/$n.env")
	env $envs ICONSDL_TEST_EVENTS="$script" "$tmp/timeout" 30 "$GCON" run "$n.icn" >"$n.out" 2>&1 ||
		echo "exit status $?" >>"$n.out"
	if cmp -s "$d/$n.std" "$n.out"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $n"
		diff "$d/$n.std" "$n.out" | head -10
	fi
done
echo "window: $pass pass, $fail fail"
[ $fail -eq 0 ]
