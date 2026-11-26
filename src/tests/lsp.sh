#!/bin/sh
# The language server's test: gcon lsp (built here with the address and
# undefined-behaviour sanitizers) against iconls, the server it is
# ported from (icontools, beside the checkout: ../icontools, or
# $ICONTOOLS; gcon never imports it, it only runs it), on sessions made
# by tests/lspsession.c for each .icn file of the corpus: the replies
# and notifications must be the same, message for message, but for the
# differences that are gcon's by design, normalised here: the server's
# name and version and the diagnostics' source; gcon's related
# information on a redeclaration; and the wording of name resolution's
# diagnostics, which is icont's in gcon (docs/design.md).
#
#   src/tests/lsp.sh [basic|features] [file ...]
#
# basic (the default): open, diagnostics, document symbols (both
# forms), formatting. features: also hover, definition, references,
# highlights, signature help, completion and rename at identifiers
# (every STEPth, default 7), and workspace symbols. Both servers search
# gcon's library for linked files (iconls through its ipath option, gcon
# through its root, iconls also given the include directories, which gcon
# finds in its root). VERBOSE=n shows the first n lines of a difference.
#
# The files are shared among $JOBS workers (default: one per CPU). gcon
# is built optimised, or with the sanitizers if SANITIZE=1. The flat
# form of document symbols is checked on every tenth file, or on all if
# FULL=1.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
tools=${ICONTOOLS:-$here/../icontools}
[ -d "$tools/cmd/iconls" ] || { echo "lsp.sh: no icontools at $tools" >&2; exit 2; }
mode=basic
case "${1:-}" in basic|features) mode=$1; shift ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
(cd "$tools" && go build -o "$tmp/iconls" ./cmd/iconls) || exit 2
if [ "${SANITIZE:-}" = 1 ]; then
	flags="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
else
	flags="-O2"
fi
$CC -std=c11 $flags -DGCON_VERSION='"test"' -I"$here/vm/src" -o "$tmp/gcon" "$here"/src/*.c \
	"$here/vm/src/gen_tables.c" || exit 2
$CC -std=c11 -O1 -I"$here/vm/src" -o "$tmp/lspsession" "$here/src/tests/lspsession.c" "$here/src/json.c" \
	"$here/src/util.c" "$here/src/token.c" "$here/src/lexer.c" || exit 2
GCONROOT=$here
LC_ALL=C
export GCONROOT LC_ALL
unset IPATH LPATH
lib="$here/lib/ipl/procs $here/lib/ipl/gprocs $here/lib/ipl/incl $here/lib/ipl/gincl"

if [ $# -gt 0 ]; then
	for f; do echo "$f"; done >"$tmp/list"
else
	{
		[ -d "$here/../icon" ] && find "$here/../icon" -name '*.icn' -not -path '*/.git/*'
		find "$here/lib" "$here/internal" "$here/experiments" "$here/vm/tests" "$here/src/tests" -name '*.icn'
		[ -d "$here/../icn" ] && find "$here/../icn" -name '*.icn'
	} | sort >"$tmp/list"
fi

# normalise: the differences by design
norm() {
	sed -e 's/"serverInfo":{"name":"iconls","version":"[^"]*"}/"serverInfo":SERVER/' \
	    -e 's/"serverInfo":{"name":"gcon","version":"[^"]*"}/"serverInfo":SERVER/' \
	    -e 's/"source":"iconls"/"source":"SRC"/g; s/"source":"gcon"/"source":"SRC"/g' \
	    -e 's/,"relatedInformation":\[{"location":{"uri":"[^"]*","range":{"start":{"line":[0-9]*,"character":[0-9]*},"end":{"line":[0-9]*,"character":[0-9]*}}},"message":"[^"]*"}\]//g' \
	    -e 's/: duplicate field name"/: redeclared identifier"/g' \
	    -e 's/: inconsistent redeclaration (also a [a-z ]* in [^)]*)"/: inconsistent redeclaration"/g' \
	    -e 's/"message":"\\u0026\([A-Za-z_0-9]*\): invalid keyword"/"message":"\\"\1\\": invalid keyword"/g'
}

# Files whose remaining differences are by design and cannot be
# normalised: src/tests/sema/redecl.icn declares locals of different
# kinds under one name, which icont (and gcon) call an inconsistent
# redeclaration, and iconls a redeclared identifier.
known="src/tests/sema/redecl.icn"

# work LIST OUT: compare the sessions of LIST's files, writing "same N"
# (N messages) or "diff FILE" lines, and any differences, to OUT.
work() {
	w=$2.tmp
	n=0
	while read -r f; do
		n=$((n + 1))
		for k in $known; do
			case "$f" in *"$k") echo "known $f"; continue 2 ;; esac
		done
		for h in 1 0; do
			if [ $mode = basic ]; then
				[ $h = 0 ] && [ "${FULL:-}" != 1 ] && [ $((n % 10)) != 0 ] && continue
				"$tmp/lspsession" basic "$f" $h $lib >"$w.sess" || continue
			else
				[ $h = 0 ] && continue
				"$tmp/lspsession" features "$f" "${STEP:-7}" $lib >"$w.sess" || continue
			fi
			"$tmp/iconls" <"$w.sess" 2>/dev/null | "$tmp/lspsession" split | norm >"$w.i"
			"$tmp/gcon" lsp <"$w.sess" 2>"$w.err" | "$tmp/lspsession" split | norm >"$w.c"
			if cmp -s "$w.i" "$w.c" && ! [ -s "$w.err" ]; then
				echo "same $(wc -l <"$w.c")"
			else
				echo "diff $f (hierarchical $h)"
				if [ "${VERBOSE:-}" ]; then
					diff "$w.i" "$w.c" | cut -c1-400 | head -"$VERBOSE" | sed 's/^/| /'
					head -3 "$w.err" | sed 's/^/| /'
				fi
			fi
		done
	done <"$1" >"$2"
}

jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
awk -v n="$jobs" -v d="$tmp" '{ print > (d "/part" (NR % n)) }' "$tmp/list"
for part in "$tmp"/part*; do
	work "$part" "$part.out" &
done
wait
cat "$tmp"/part*.out >"$tmp/results"
grep '^| ' "$tmp/results" | sed 's/^| //'
grep '^diff ' "$tmp/results" | sed 's/^diff /DIFF /'
same=$(grep -c '^same ' "$tmp/results")
msgs=$(awk '/^same / { n += $2 } END { print n + 0 }' "$tmp/results")
fail=$(grep -c '^diff ' "$tmp/results")
nknown=$(grep -c '^known ' "$tmp/results")
echo "lsp $mode: $same sessions identical ($msgs messages), $fail differ; $nknown known by design"
[ "$fail" -eq 0 ]
