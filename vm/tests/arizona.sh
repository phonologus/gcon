#!/bin/sh
# Graphics programs under gcon and under Arizona Icon's own iconx, built
# with its X11 graphics (tools/arizona/build.sh, from ../icon): each runs
# for TIMEOUT seconds (default 20) with no input, both on one display of
# their own (a private Xvfb, XQuartz's on macOS, else $DISPLAY), gcon
# with its X11 server, whose drawing is X's own; and the two runs'
# output and exit status must be the same. Their pictures are not
# compared here (Arizona's iconx has no way to give them up): ref.sh
# compares drawing with Arizona's images.
#
#   tests/arizona.sh [prog.icn ...]
#
# By default, the programs gcon ships (../lib/ipl/gprogs,
# ../lib/ipl/gpacks/vib); each runs in a copy of its directory (for
# iconx, its other files translated to ucode, for its links). A program
# whose output depends on the clock or chance differs from itself, and
# so from gcon (VERBOSE=1 keeps the outputs, in tests/arizona.out).
#
# Needs Arizona's tree beside gcon's checkout (../icon), Xlib and libXpm,
# and gcon ($GCON, or built from $GCONSRC, default this checkout).
# Without them, or without Xvfb and $DISPLAY, it says so and does nothing.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
if [ ! -d "$src/../icon/src" ]; then
	echo "arizona: no Arizona tree at $src/../icon: nothing compared"
	exit 0
fi
if ! az=$(sh "$src/tools/arizona/build.sh"); then
	echo "arizona: Arizona Icon did not build with X11: nothing compared"
	exit 0
fi
if ! sh "$src/graphics/x11/build.sh" -check; then
	echo "arizona: no Xlib: nothing compared"
	exit 0
fi
sh "$src/graphics/x11/build.sh" >/dev/null || exit 2
tmp=$(mktemp -d)
xvfb=
trap '[ -n "$xvfb" ] && kill $xvfb 2>/dev/null; rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
GCONROOT=${GCONROOT:-$src}
export GCONROOT
export GCONGRAPHICS=$src/graphics/bin/gcon-graphics-x11
export GCONVM=${GCONVM:-$here/bin/gcon-vm}
T=${TIMEOUT:-20}

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
	[ -e "/tmp/.X11-unix/X$n" ] || { echo "arizona: Xvfb did not start: $(cat "$tmp/xvfb.log")"; exit 2; }
elif [ -z "${DISPLAY:-}" ]; then
	echo "arizona: no Xvfb and no DISPLAY: nothing compared"
	exit 0
fi
export DISPLAY

[ $# -gt 0 ] || set -- "$src"/lib/ipl/gprogs/*.icn "$src/lib/ipl/gpacks/vib/vib.icn"
out=$tmp/out
[ "${VERBOSE:-}" ] && out=$here/tests/arizona.out && rm -rf "$out"
same=0 differ=0
for f in "$@"; do
	n=$(basename "$f" .icn)
	d=$(cd "$(dirname "$f")" && pwd)
	w=$out/$n
	for side in g a; do
		mkdir -p "$w/$side"
		(cd "$d" && tar cf - --exclude='*.u1' --exclude='*.u2' .) | (cd "$w/$side" && tar xf -)
	done
	# the two runs at once
	(cd "$w/g" && "$tmp/timeout" -term "$T" "$GCON" run "$n.icn" </dev/null >out 2>&1
		echo "status $?" >>out) &
	gp=$!
	(cd "$w/a" && export IPATH="$w/a" &&
		for x in *.icn; do grep -q "^procedure main" "$x" || "$az/bin/icont" -s -c "$x" >/dev/null 2>&1; done
		if "$az/bin/icont" -s -o "$n" "$n.icn" >build 2>&1; then
			"$tmp/timeout" -term "$T" ./"$n" </dev/null >out 2>&1
			echo "status $?" >>out
		else
			grep "^File" build >out
			echo "status 1" >>out
		fi) &
	ap=$!
	wait $gp $ap # not Xvfb's
	if cmp -s "$w/g/out" "$w/a/out"; then
		same=$((same + 1))
	else
		differ=$((differ + 1))
		echo "DIFF $n: output differs"
		diff "$w/a/out" "$w/g/out" | head -6 | sed 's/^/  /'
	fi
done
echo "arizona: $same programs the same as Arizona's iconx, $differ differ"
[ $differ -eq 0 ]
