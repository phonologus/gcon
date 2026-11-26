#!/bin/sh
# Runs programs on gcon-vm and compares each with its recorded run:
# standard output, standard error and exit status, kept in
# tests/expected/SUITE/NAME.out and NAME.err (SUITE is the program's
# directory and its parent, as "tests-general"). FILE.dat beside a
# program, if present, is its standard input.
#
#   tests/expected.sh [file.icn | dir] ...          check (default:
#                                                   ../../icon/tests/general
#                                                   ../../icon/ipl/progs)
#   tests/expected.sh -record-go [file | dir] ...   record gcon's Go VM
#   tests/expected.sh -accept file.icn ...          record gcon-vm
#
# The recordings were made from gcon's Go VM while it was the reference
# (-record-go), each program run twice: a program whose two runs differ
# (table order, times), that does not finish within TIMEOUT seconds
# (default 10), or that gcon cannot build is marked NAME.skip with the
# reason, and not checked. Where gcon-vm differs from the Go VM by
# decision (features the Go VM lacks), its run is recorded instead
# (-accept), and the reason added to tests/expected/ACCEPTED.
#
# Needs gcon (as tests/dis.sh).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
exp=$here/tests/expected
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
mode=check
case "${1:-}" in
-record-go) mode=record; shift ;;
-accept) mode=accept; shift ;;
esac
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
t=${TIMEOUT:-10}
[ $# -gt 0 ] || set -- "$here/../../icon/tests/general" "$here/../../icon/ipl/progs"
for arg in "$@"; do
	if [ -d "$arg" ]; then find "$arg" -name '*.icn'; else echo "$arg"; fi
done | sort >"$tmp/list"

pass=0 fail=0 skip=0 none=0 recorded=0

# run DIR NAME OUT VM...: build NAME.icn in DIR and run it in a fresh
# directory as ./NAME, output in OUT.out and OUT.err (with the status).
# Status 2: not built; 142: timed out.
run() {
	d=$1 name=$2 o=$3
	shift 3
	w=$tmp/w && rm -rf "$w" && mkdir -p "$w"
	in=/dev/null
	[ -f "$d/$name.dat" ] && in=$d/$name.dat
	if ! (cd "$d" && "$GCON" build -o "$w/$name" "$name.icn") >/dev/null 2>&1; then
		return 2
	fi
	(cd "$w" && "$tmp/timeout" "$t" "$@" "./$name" <"$in" >"$o.out" 2>"$o.err")
	s=$?
	echo "status $s" >>"$o.err"
	[ $s -eq 142 ] && return 142
	return 0
}

while read -r f; do
	d=$(cd "$(dirname "$f")" && pwd)
	name=$(basename "$f" .icn)
	suite=$(basename "$(dirname "$d")")-$(basename "$d")
	e=$exp/$suite/$name
	case $mode in
	record)
		mkdir -p "$exp/$suite"
		rm -f "$e.out" "$e.err" "$e.skip"
		run "$d" "$name" "$tmp/a" env GCONVM=go "$GCON" run
		s=$?
		if [ $s -eq 2 ]; then
			echo "not built by gcon" >"$e.skip"
		elif [ $s -eq 142 ]; then
			echo "does not finish within ${t}s on the Go VM" >"$e.skip"
		else
			run "$d" "$name" "$tmp/b" env GCONVM=go "$GCON" run
			if cmp -s "$tmp/a.out" "$tmp/b.out" && cmp -s "$tmp/a.err" "$tmp/b.err"; then
				cp "$tmp/a.out" "$e.out"
				cp "$tmp/a.err" "$e.err"
				recorded=$((recorded + 1))
				continue
			fi
			echo "nondeterministic: two runs on the Go VM differ" >"$e.skip"
		fi
		skip=$((skip + 1))
		;;
	accept)
		mkdir -p "$exp/$suite"
		if run "$d" "$name" "$tmp/c" "$vm"; then
			rm -f "$e.skip"
			cp "$tmp/c.out" "$e.out"
			cp "$tmp/c.err" "$e.err"
			recorded=$((recorded + 1))
			echo "accepted $suite/$name"
		else
			echo "could not run $f" >&2
			fail=$((fail + 1))
		fi
		;;
	check)
		if [ -f "$e.skip" ]; then
			skip=$((skip + 1))
			continue
		fi
		if [ ! -f "$e.out" ]; then
			none=$((none + 1))
			[ "${VERBOSE:-}" ] && echo "unrecorded $f"
			continue
		fi
		run "$d" "$name" "$tmp/c" "$vm"
		if cmp -s "$e.out" "$tmp/c.out" && cmp -s "$e.err" "$tmp/c.err"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
			echo "FAIL $f"
			if [ "${VERBOSE:-}" ]; then
				diff -a "$e.out" "$tmp/c.out" | head -"$VERBOSE"
				diff -a "$e.err" "$tmp/c.err" | head -"$VERBOSE"
			fi
		fi
		;;
	esac
done <"$tmp/list"

case $mode in
record) echo "expected: $recorded recorded from the Go VM, $skip skipped" ;;
accept) echo "expected: $recorded accepted from gcon-vm" ;;
check)
	echo "expected: $pass pass, $fail fail, $skip skipped, $none unrecorded"
	[ $fail -eq 0 ] || exit 1
	;;
esac
