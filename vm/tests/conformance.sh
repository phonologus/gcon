#!/bin/sh
# The conformance test: Icon's own test programs (../../icon/tests/general,
# or $ICON_SRC/tests/general), run as their Test-icon script runs them,
# in a copy of their directory: each built by gcon and run with FILE.dat
# (if present) as standard input, its standard output and error together
# compared with FILE.std, iconx's.
#
#   tests/conformance.sh            check
#   tests/conformance.sh -update    rewrite tests/conformance.txt
#
# tests/conformance.txt lists the tests gcon passes: a listed test that
# fails is a regression, and a passing test not listed must be added
# (-update), so the list records progress exactly. Needs gcon ($GCON, or
# built from $GCONSRC, default this checkout). TIMEOUT (seconds, default
# 5) limits each.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
list=$here/tests/conformance.txt
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
update=
[ "${1:-}" = -update ] && update=1
icon=${ICON_SRC:-$here/../../icon}
[ -d "$icon/tests/general" ] || { echo "conformance: no $icon/tests/general" >&2; exit 2; }
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
if [ -z "${GCON:-}" ]; then
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
# link finds the Icon Program Library in gcon's root, as iconx's tests
# find it built in icon/lib.
GCONROOT=${GCONROOT:-$src}
export GCONROOT
unset IPATH LPATH BLKSIZE STRSIZE MSTKSIZE COEXPSIZE QLSIZE
cp -R "$icon/tests/general" "$tmp/general"
# icon-sdl's one change to the suite, for gcon's backend: "SDL graphics"
# is, like "X Windows", a feature that may be absent. The line must occur
# once, or upstream has changed and this needs looking at again.
old='   every delete(f, "graphics" | "X Windows")			# may be absent'
new='   every delete(f, "graphics" | "X Windows" | "SDL graphics")	# may be absent'
if [ "$(grep -cxF -- "$old" "$tmp/general/kwds.icn")" != 1 ]; then
	echo "conformance: kwds.icn has changed upstream; look at its change again" >&2
	exit 2
fi
awk -v old="$old" -v new="$new" '$0 == old { print new; next } { print }' "$tmp/general/kwds.icn" >"$tmp/kwds.icn"
mv "$tmp/kwds.icn" "$tmp/general/kwds.icn"
cd "$tmp/general" || exit 2
t=${TIMEOUT:-5}
pass=0 fail=0 total=0
: >"$tmp/passed"
for std in *.std; do
	name=${std%.std}
	[ -f "$name.icn" ] || continue
	total=$((total + 1))
	in=/dev/null
	[ -f "$name.dat" ] && in=$name.dat
	ok=
	if "$GCON" build -o "$name" "$name.icn" >/dev/null 2>&1; then
		"$tmp/timeout" "$t" "./$name" <"$in" >"$tmp/out" 2>&1
		cmp -s "$std" "$tmp/out" && ok=1
		rm -f "$name"
	fi
	listed=
	grep -qx "$name" "$list" && listed=1
	if [ "$ok" ]; then
		echo "$name" >>"$tmp/passed"
		pass=$((pass + 1))
		if [ -z "$listed" ] && [ -z "$update" ]; then
			echo "$name passes but is not listed in tests/conformance.txt"
			fail=$((fail + 1))
		fi
	elif [ "$listed" ]; then
		echo "FAIL $name (listed in tests/conformance.txt)"
		fail=$((fail + 1))
	fi
done
if [ "$update" ]; then
	{
		echo "# Reference tests (icon/tests/general) that gcon passes."
		echo "# Maintained by: tests/conformance.sh -update"
		cat "$tmp/passed"
	} >"$list"
fi
echo "conformance: $pass of $total reference tests pass; $fail problems"
[ "$fail" -eq 0 ]
