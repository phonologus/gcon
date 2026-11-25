#!/bin/sh
# The differential test: gcon builds each program given (directories
# are searched for *.icn), and gcon-vm must run the built file exactly
# as gcon's Go VM runs it: the same standard output, standard error and
# exit status. FILE.dat, if present, is standard input. The Go VM runs
# each program twice; one whose two runs differ (table order, times) is
# reported as nondeterministic and not compared.
#
#   tests/vs-go.sh ../icon/tests/general ../icon/ipl/progs
#
# Needs gcon, as tests/dis.sh. TIMEOUT (seconds, default 10) limits each
# run; a program the Go VM does not finish in time is skipped.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/../gcon}" && pwd) || exit 2
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
t=${TIMEOUT:-10}
pass=0 fail=0 skip=0 nondet=0 slow=0
for arg in "$@"; do
	if [ -d "$arg" ]; then find "$arg" -name '*.icn'; else echo "$arg"; fi
done | sort >"$tmp/list"
while read -r f; do
	dir=$(cd "$(dirname "$f")" && pwd)
	name=$(basename "$f" .icn)
	in=/dev/null
	[ -f "$dir/$name.dat" ] && in=$dir/$name.dat
	w=$tmp/w && rm -rf "$w" && mkdir -p "$w/a" "$w/b" "$w/c"
	if ! (cd "$dir" && "$GCON" build -o "$w/prog" "$name.icn") >/dev/null 2>&1; then
		skip=$((skip + 1))
		continue
	fi
	for k in a b; do
		cp "$w/prog" "$w/$k/$name"
		(cd "$w/$k" && "$here/tests/timeout" "$t" "$GCON" run "./$name" <"$in" >out 2>err)
		echo "status $?" >>"$w/$k/err"
	done
	if grep -q "^status 142" "$w/a/err"; then
		slow=$((slow + 1))
		continue
	fi
	if ! cmp -s "$w/a/out" "$w/b/out" || ! cmp -s "$w/a/err" "$w/b/err"; then
		nondet=$((nondet + 1))
		continue
	fi
	cp "$w/prog" "$w/c/$name"
	(cd "$w/c" && "$here/tests/timeout" "$t" "$vm" "./$name" <"$in" >out 2>err)
	echo "status $?" >>"$w/c/err"
	if cmp -s "$w/a/out" "$w/c/out" && cmp -s "$w/a/err" "$w/c/err"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $f"
		if [ "${VERBOSE:-}" ]; then
			diff -a "$w/a/out" "$w/c/out" | head -"$VERBOSE"
			diff -a "$w/a/err" "$w/c/err" | head -"$VERBOSE"
		fi
	fi
done <"$tmp/list"
echo "vs-go: $pass identical, $fail differ, $nondet nondeterministic, $slow too slow, $skip not built"
[ "$fail" -eq 0 ]
