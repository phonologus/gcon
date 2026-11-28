#!/bin/sh
# The reference tests: programs from icon-sdl's sdl/tests/ref, each
# drawing on a hidden canvas and writing it with WriteImage, compared
# with the outputs of unmodified Arizona Icon built with X11 graphics
# (tests/ref/expected; see its SOURCE). Standard output must match
# exactly, and each image may differ by no more pixels than
# its server's tolerance file allows: tests/ref/tolerance for the SDL
# server (icon-sdl's backend, with its accepted differences from X11),
# tolerance-cocoa and tolerance-x11 for the Cocoa and X11 servers (chosen
# by the server's -version). The server is $GCONGRAPHICS, by default the
# one build.sh installed (../libexec/gcon-graphics); without one it says
# so and checks nothing.
#
#   tests/ref.sh [OUTDIR]
#
# REFTOLERANCE names another tolerance file.
#
# OUTDIR (default: a temporary directory) keeps the outputs, with
# NAME.diff.png for each image that differs. Needs gcon ($GCON, or
# built from $GCONSRC, default this checkout) and the display server.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
ref=$here/tests/ref
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out=${1:-$tmp/out}
mkdir -p "$out"
${CC:-cc} -std=c11 -O1 -o "$tmp/compare" "$ref/compare.c" || exit 2
src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
GCONROOT=${GCONROOT:-$src}
export GCONROOT
export GCONGRAPHICS=${GCONGRAPHICS:-$src/libexec/gcon-graphics}
if [ ! -x "$GCONGRAPHICS" ]; then
	echo "ref: no display server ($GCONGRAPHICS): not checked"
	exit 0
fi
# each server's accepted differences from the X11 reference
case $("$GCONGRAPHICS" -version 2>/dev/null) in
*X11*) REFTOLERANCE=${REFTOLERANCE:-$ref/tolerance-x11} ;;
*Cocoa*) REFTOLERANCE=${REFTOLERANCE:-$ref/tolerance-cocoa} ;;
esac
export GCONVM=${GCONVM:-$here/bin/gcon-vm}

# tolerance NAME FIELD: the accepted difference from the tolerance file
tolerance() {
	awk -v n="$1" -v f="$2" '$1 == n { print $f; found = 1 }
		END { if (!found) print (f == 2 ? 0 : "same") }' "${REFTOLERANCE:-$ref/tolerance}"
}

bad=0
cd "$out" || exit 2
for f in "$ref"/*.icn; do
	n=$(basename "$f" .icn)
	maxpix=$(tolerance "$n" 2)
	outrule=$(tolerance "$n" 3)
	cp "$f" .
	rm -f "$n.gif"
	"$GCON" run "$n.icn" "$n.gif" >"$n.out" 2>&1 || echo "$n: exit status $?" >>"$n.out"
	if cmp -s "$ref/expected/$n.out" "$n.out"; then o="out same"
	elif [ "$outrule" = differs ]; then o="out differs (ok)"
	else o="out DIFFERS"; bad=1; fi
	if [ -f "$n.gif" ]; then
		g=$("$tmp/compare" "$ref/expected/$n.gif" "$n.gif" "$n.diff.png") || true
		npix=${g%% *}
		case "$npix" in
		*[!0-9]* | "") verdict="ERROR"; bad=1 ;;
		*)
			if [ "$maxpix" -lt 0 ] || [ "$npix" -le "$maxpix" ]; then verdict="ok"
			else verdict="FAIL (allowed $maxpix)"; bad=1; fi
			;;
		esac
	else
		g="no image"
		verdict="FAIL"
		bad=1
	fi
	printf '%-10s %-17s %-40s %s\n' "$n" "$o" "$g" "$verdict"
done
[ $bad -eq 0 ] && echo "ref: all within tolerance" || echo "ref: FAILED"
exit $bad
