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
# The reference pages both servers read for hovers (DOCS; default none).
docs=${DOCS:-$tmp/nodocs}
lib="$here/lib/ipl/procs $here/lib/ipl/gprocs $here/lib/ipl/incl $here/lib/ipl/gincl"

named=
if [ $# -gt 0 ]; then
	named=1
	for f; do echo "$f"; done >"$tmp/list"
else
	{
		[ -d "$here/../icon" ] && find "$here/../icon" -name '*.icn' -not -path '*/.git/*'
		find "$here/lib" "$here/experiments" "$here/vm/tests" "$here/src/tests" -name '*.icn'
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
	    -e 's/"message":"\\u0026\([A-Za-z_0-9]*\): invalid keyword"/"message":"\\"\1\\": invalid keyword"/g' |
	# gcon's built-ins and keywords that Icon 9.5 lacks, in completions
	sed -E -e 's/,\{"label":"(bytes|len|runes|uchar|utf8valid)","kind":3,"detail":"[^"]*","documentation":\{"kind":"markdown","value":"[^"]*"\},"sortText":"2[a-z0-9]*"\}//g' \
	    -e 's/,\{"label":"uset","kind":21,"detail":"\\u0026uset","documentation":\{"kind":"markdown","value":"[^"]*"\}\}//g'
}

# Files whose remaining differences are by design and cannot be
# normalised: src/tests/sema/redecl.icn declares locals of different
# kinds under one name, which icont (and gcon) call an inconsistent
# redeclaration, and iconls a redeclared identifier; src/tests/lit/*.icn
# use gcon's literals, which iconls does not know (checked below).
known="src/tests/sema/redecl.icn src/tests/lit/literals.icn"

# firstdiff A B: for each message that differs, its number and the text
# of both around the first differing byte.
firstdiff() {
	awk 'NR == FNR { a[FNR] = $0; n = FNR; next }
	{
		if (a[FNR] == $0) next
		x = a[FNR]; i = 1
		while (i <= length(x) && substr(x, i, 1024) == substr($0, i, 1024)) i += 1024
		while (i <= length(x) && substr(x, i, 1) == substr($0, i, 1)) i++
		s = i > 60 ? i - 60 : 1
		print "message " FNR " at byte " i ":"
		print "< " substr(x, s, 200)
		print "> " substr($0, s, 200)
	}
	END { if (FNR != n) print "message counts " n " and " FNR }' "$1" "$2"
}

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
				[ "${FULL:-}" != 1 ] && [ -z "$named" ] && [ $((n % 10)) != 0 ] && continue
				"$tmp/lspsession" features "$f" "${STEP:-7}" $lib >"$w.sess" || continue
			fi
			"$tmp/iconls" -docs "$docs" <"$w.sess" 2>/dev/null | "$tmp/lspsession" split | norm >"$w.i"
			"$tmp/gcon" lsp -docs "$docs" <"$w.sess" 2>"$w.err" | "$tmp/lspsession" split | norm >"$w.c"
			if cmp -s "$w.i" "$w.c" && ! [ -s "$w.err" ]; then
				echo "same $(wc -l <"$w.c")"
			else
				echo "diff $f (hierarchical $h)"
				if [ "${VERBOSE:-}" ]; then
					firstdiff "$w.i" "$w.c" | head -"$VERBOSE" | sed 's/^/| /'
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

# gcon's additions, whose oracle is gcon itself: the diagnostics for
# the literal probes are the errors gcon build reports.
lfail=0
for f in "$here"/src/tests/lit/*.icn; do
	(cd "$(dirname "$f")" && "$tmp/gcon" build -o /dev/null "$(basename "$f")") 2>&1 |
		sed 's/^File [^;]*; Line //' >"$tmp/lit.build"
	"$tmp/lspsession" basic "$f" 1 | "$tmp/gcon" lsp 2>/dev/null | "$tmp/lspsession" diags >"$tmp/lit.lsp"
	if ! cmp -s "$tmp/lit.build" "$tmp/lit.lsp"; then
		echo "DIFF $f (diagnostics and gcon build)"
		diff "$tmp/lit.build" "$tmp/lit.lsp"
		lfail=$((lfail + 1))
	fi
done
echo "lsp literals: $(ls "$here"/src/tests/lit/*.icn | wc -l | tr -d ' ') probes, $lfail differ from gcon build"
[ "$fail" -eq 0 ] && [ "$lfail" -eq 0 ]
