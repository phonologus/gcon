#!/bin/sh
# Runs programs on gcon-vm and compares each with its recorded run:
# standard output, standard error and exit status, kept in
# tests/expected/SUITE/NAME.out and NAME.err (SUITE is the program's
# directory and its parent, as "tests-general"). FILE.dat beside a
# program, if present, is its standard input.
#
#   tests/expected.sh [file.icn | dir] ...          check (default:
# ../../icon/tests/general
#                                                   ../../icon/ipl/progs)
#   tests/expected.sh -accept file.icn ...          record gcon-vm
#
# The recordings were made from gcon's Go VM while it was the reference
# (the tag go-reference), each program run twice: a program whose two
# runs differ (table order, times), that did not finish within 10
# seconds, or that gcon cannot build is marked NAME.skip with the
# reason, and not checked. Where gcon-vm differs from the Go VM by
# decision (features the Go VM lacked), its run is recorded instead
# (-accept), and the reason added to tests/expected/ACCEPTED; new
# programs are recorded the same way, once their runs are checked
# against iconx. TIMEOUT (seconds, default 10) limits each run.
#
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
exp=$here/tests/expected
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O1 -o "$tmp/timeout" "$here/tests/timeout.c" || exit 2
mode=check
case "${1:-}" in
-accept) mode=accept; shift ;;
esac
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
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
accept) echo "expected: $recorded accepted from gcon-vm" ;;
check)
	echo "expected: $pass pass, $fail fail, $skip skipped, $none unrecorded"
	[ $fail -eq 0 ] || exit 1
	;;
esac
