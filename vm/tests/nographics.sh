#!/bin/sh
# gcon -nographics: tests/nographics.icn, built with gcon build
# -nographics and run, and translated and run by gcon run -nographics,
# must give tests/nographics.std, with no display server ($GCONGRAPHICS
# names none: a program that started one would say so); and linking the
# graphics library must be refused when the program is translated.
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "${GCONSRC:-$here/..}" && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c"
	GCON=$tmp/gcon
fi
export GCONROOT=$src GCONVM=$vm GCONGRAPHICS=$tmp/no-display-server
fail=0
problem() { echo "FAIL $*"; fail=1; }
cp "$here/tests/nographics.icn" "$tmp"
(cd "$tmp" && "$GCON" build -nographics -o nographics nographics.icn)
(cd "$tmp" && ./nographics >out 2>&1) || true
diff "$here/tests/nographics.std" "$tmp/out" || problem "gcon build -nographics: not tests/nographics.std"
(cd "$tmp" && "$GCON" run -nographics nographics.icn >run.out 2>&1) || true
diff "$here/tests/nographics.std" "$tmp/run.out" || problem "gcon run -nographics: not tests/nographics.std"
printf 'link graphics\nprocedure main()\nend\n' >"$tmp/lk.icn"
if (cd "$tmp" && "$GCON" build -nographics -o lk lk.icn) >"$tmp/lk.out" 2>&1; then
	problem "gcon build -nographics: linked the graphics library"
fi
grep -q "graphics.icn" "$tmp/lk.out" || problem "link graphics: $(cat "$tmp/lk.out")"
(cd "$tmp" && "$GCON" build -o lk lk.icn) >"$tmp/lk.out" 2>&1 || problem "link graphics (with graphics): $(cat "$tmp/lk.out")"
[ $fail = 0 ] && echo "nographics: pass" || echo "nographics: FAIL"
exit $fail
