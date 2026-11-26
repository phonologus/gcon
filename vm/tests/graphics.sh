#!/bin/sh
# Runs tests/graphics.icn (graphics on a hidden canvas, so no display is
# needed) and compares its output with tests/graphics.std. Needs gcon
# ($GCON, or built from $GCONSRC, default this checkout) and the display
# server: $GCONGRAPHICS, by default ../graphics/bin/gcon-graphics in this
# checkout.
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
export GCONGRAPHICS=${GCONGRAPHICS:-$here/../graphics/bin/gcon-graphics}
cp "$here/tests/graphics.icn" "$tmp"
(cd "$tmp" && "$GCON" build -o graphics graphics.icn)
(cd "$tmp" && "$vm" ./graphics >out 2>&1) || true
if diff "$here/tests/graphics.std" "$tmp/out"; then
	echo "graphics: pass"
else
	echo "graphics: FAIL"
	exit 1
fi
