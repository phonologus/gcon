#!/bin/sh
# Build Arizona Icon, the reference implementation, with its X11 graphics,
# in a private copy of the tree beside gcon's checkout (../icon, which is
# never modified), for the tests that run programs under its iconx
# (vm/tests/arizona.sh). Needs Xlib and libXpm (XQuartz's on macOS).
# Prints the built tree's directory: bin/icont, bin/iconx, its programs
# (bin/vib, ...) and lib/, its IPL translated.
#
#   tools/arizona/build.sh [ICON_SRC] [WORKDIR]
#
# ICON_SRC defaults to ../icon (relative to the repo root); WORKDIR
# (default: $TMPDIR/gcon-arizona-x11) holds the copy, built once.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=${1:-$here/../../../icon}
work=${2:-${TMPDIR:-/tmp}/gcon-arizona-x11}
case $(uname) in Darwin) cfg=macintosh ;; *) cfg=linux ;; esac
if [ ! -x "$work/bin/iconx" ] || [ ! -f "$work/lib/graphics.u1" ]; then
	rm -rf "$work"
	mkdir -p "$work"
	(cd "$src" && tar --exclude=.git -cf - .) | (cd "$work" && tar xf -)
	(cd "$work" && make X-Configure name=$cfg >configure.log 2>&1 && make >build.log 2>&1) || {
		echo "arizona/build.sh: Arizona Icon did not build with X11 (see $work/build.log)" >&2
		exit 1
	}
fi
echo "$work"
