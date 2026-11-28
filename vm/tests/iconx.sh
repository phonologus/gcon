#!/bin/sh
# Graphics programs under gcon and under icon-sdl's iconx, whose SDL3 +
# Blend2D backend gcon's display server is: each runs for TIMEOUT
# seconds (default 20) with no input, and the two runs' output, exit
# status and every canvas must be the same, pixel for pixel. The
# canvases come from a test hook in both servers: with
# ICONSDL_TEST_DUMP=dir, each canvas is written as dir/NNN.ppm when it
# is freed and at exit (graphics/src/canvas.c).
#
#   tests/iconx.sh [prog.icn ...]
#
# Each program runs in a copy of its directory (for iconx, the other
# files there translated to ucode, for its links); by default, the
# programs gcon ships (../lib/ipl/gprogs, ../lib/ipl/gpacks/vib). A
# program whose output depends on the clock or chance (randomize,
# animation) differs from itself, and so from gcon: run iconx twice to
# tell (VERBOSE=1 keeps the outputs, in tests/iconx.out).
#
# Needs icon-sdl built beside gcon's checkout (../../icon-sdl, or
# $ICONSDL), with its dump hook, a display, and gcon ($GCON, or built
# from $GCONSRC, default this checkout). Without icon-sdl it says so
# and does nothing.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
isdl=${ICONSDL:-$src/../icon-sdl}
if ! [ -x "$isdl/bin/iconx" ] || ! grep -q ICONSDL_TEST_DUMP "$isdl/bin/iconx" 2>/dev/null; then
	echo "iconx: no icon-sdl iconx with the dump hook at $isdl: nothing compared"
	exit 0
fi
isdl=$(cd "$isdl" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
${CC:-cc} -std=c11 -O1 -o "$tmp/ppmcmp" "$here/tests/ppmcmp.c" || exit 2
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
GCONROOT=${GCONROOT:-$src}
export GCONROOT
export GCONGRAPHICS=${GCONGRAPHICS:-$src/graphics/bin/gcon-graphics}
if [ ! -x "$GCONGRAPHICS" ]; then
	echo "iconx: no SDL display server ($GCONGRAPHICS; ./build.sh --withSDL): nothing compared"
	exit 0
fi
export GCONVM=${GCONVM:-$here/bin/gcon-vm}
T=${TIMEOUT:-20}

# icon-sdl's IPL as ucode, for iconx's links
mkdir "$tmp/ucode"
(cd "$tmp/ucode" && for f in "$isdl"/ipl/procs/*.icn "$isdl"/ipl/gprocs/*.icn; do
	cp "$f" . && LPATH="$isdl/ipl/incl $isdl/ipl/gincl" "$isdl/bin/icont" -s -c "$(basename "$f")" >/dev/null 2>&1
	rm -f "$(basename "$f")"
done)

[ $# -gt 0 ] || set -- "$src"/lib/ipl/gprogs/*.icn "$src/lib/ipl/gpacks/vib/vib.icn"
out=$tmp/out
[ "${VERBOSE:-}" ] && out=$here/tests/iconx.out && rm -rf "$out"
same=0 differ=0
for f in "$@"; do
	n=$(basename "$f" .icn)
	d=$(cd "$(dirname "$f")" && pwd)
	w=$out/$n
	for side in g i; do
		mkdir -p "$w/$side/dump"
		(cd "$d" && tar cf - --exclude='*.u1' --exclude='*.u2' .) | (cd "$w/$side" && tar xf -)
	done
	# the two runs at once
	(cd "$w/g" && ICONSDL_TEST_DUMP="$w/g/dump" "$tmp/timeout" -term "$T" "$GCON" run "$n.icn" </dev/null >out 2>&1
		echo "status $?" >>out) &
	(cd "$w/i" && export IPATH="$w/i $tmp/ucode" LPATH="$isdl/ipl/incl $isdl/ipl/gincl" &&
		for x in *.icn; do grep -q "^procedure main" "$x" || "$isdl/bin/icont" -s -c "$x" >/dev/null 2>&1; done
		if "$isdl/bin/icont" -s -o "$n" "$n.icn" >build 2>&1; then
			ICONSDL_TEST_DUMP="$w/i/dump" "$tmp/timeout" -term "$T" ./"$n" </dev/null >out 2>&1
			echo "status $?" >>out
		else
			grep "^File" build >out
			echo "status 1" >>out
		fi) &
	wait
	# the server writes its dumps as it exits, after gcon-vm has gone
	if [ -f "$w/g/dump/started" ]; then
		k=0
		while [ ! -f "$w/g/dump/done" ] && [ $k -lt 120 ]; do sleep 0.5; k=$((k + 1)); done
	fi
	r=""
	cmp -s "$w/g/out" "$w/i/out" || r="output differs"
	gl=$(cd "$w/g/dump" && ls | grep -Ev '^(done|started)$')
	il=$(cd "$w/i/dump" && ls | grep -Ev '^(done|started)$')
	if [ "$gl" != "$il" ]; then
		r="$r; canvases: gcon [$(echo $gl)], iconx [$(echo $il)]"
	else
		for c in $gl; do
			p=$("$tmp/ppmcmp" "$w/i/dump/$c" "$w/g/dump/$c")
			case "$p" in "0 of"*) ;; *) r="$r; $c: $p" ;; esac
		done
	fi
	if [ -z "$r" ]; then
		same=$((same + 1))
	else
		differ=$((differ + 1))
		echo "DIFF $n: ${r#; }"
	fi
done
echo "iconx: $same programs the same as iconx's, $differ differ"
[ $differ -eq 0 ]
