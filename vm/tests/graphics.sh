#!/bin/sh
# Runs tests/graphics.icn (graphics on a hidden canvas, so no display is
# needed) and compares its output with tests/graphics.std. Needs gcon (as
# tests/dis.sh) and the display server: $GCONGRAPHICS, by default
# ../gcon-graphics/bin/gcon-graphics beside this repository.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/../gcon}" && pwd)
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon)
	GCON=$tmp/gcon
fi
export GCONGRAPHICS=${GCONGRAPHICS:-$here/../gcon-graphics/bin/gcon-graphics}
cp "$here/tests/graphics.icn" "$tmp"
(cd "$tmp" && "$GCON" build -o graphics graphics.icn)
(cd "$tmp" && "$vm" ./graphics >out 2>&1) || true
if diff "$here/tests/graphics.std" "$tmp/out"; then
	echo "graphics: pass"
else
	echo "graphics: FAIL"
	exit 1
fi
