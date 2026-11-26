#!/bin/sh
# The program tests: gcon's (internal/run/testdata, testdata/traceback),
# whose expected output, exit status and, for some, standard error were
# recorded from iconx. As gcon's TestPrograms runs them built, each is
# built by gcon as FILE in a directory of its own, with testdata/lib
# beside it, and run there as ./FILE by gcon-vm, with FILE.args as its
# arguments and FILE.dat as its standard input. Programs gcon cannot
# build (translation-error tests) are skipped.
#
#   tests/programs.sh [name ...]    # default: all
#
# Needs gcon, as tests/dis.sh. TIMEOUT (seconds, default 10) limits each.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src=$(cd "${GCONSRC:-$here/../gcon}" && pwd) || exit 2
if [ -z "${GCON:-}" ]; then
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
td=$src/internal/run/testdata
if [ $# -gt 0 ]; then
	for n in "$@"; do ls "$td/$n.icn" "$td/traceback/$n.icn" 2>/dev/null; done
else
	ls "$td"/*.icn "$td"/traceback/*.icn
fi >"$tmp/list"
pass=0 fail=0 skip=0
while read -r f; do
	base=${f%.icn}
	name=$(basename "$base")
	d=$tmp/run/$name
	rm -rf "$d" && mkdir -p "$d"
	cp "$f" "$d/" && cp -R "$td/lib/." "$d/"
	if ! (cd "$d" && "$GCON" build -o "$name" "$name.icn") >/dev/null 2>&1; then
		skip=$((skip + 1))
		continue
	fi
	args=
	[ -f "$base.args" ] && args=$(cat "$base.args")
	in=/dev/null
	[ -f "$base.dat" ] && in=$base.dat
	# shellcheck disable=SC2086
	(cd "$d" && "$here/tests/timeout" "${TIMEOUT:-10}" "$vm" "./$name" $args <"$in" >"$tmp/out" 2>"$tmp/err")
	status=$?
	want=0
	[ -f "$base.exit" ] && want=$(tr -d ' \n' <"$base.exit")
	ok=true
	cmp -s "$tmp/out" "$base.std" || ok=false
	[ "$status" = "$want" ] || ok=false
	if [ -f "$base.err" ] && ! cmp -s "$tmp/err" "$base.err"; then ok=false; fi
	if $ok; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $name (status $status, want $want)"
		if [ "${VERBOSE:-}" ]; then
			diff -a "$base.std" "$tmp/out" | head -${VERBOSE}
			[ -f "$base.err" ] && diff -a "$base.err" "$tmp/err" | head -${VERBOSE}
			head -5 "$tmp/err"
		fi
	fi
done <"$tmp/list"
echo "programs: $pass pass, $fail fail, $skip not built"
[ "$fail" -eq 0 ]
