#!/bin/sh
# The C front end against icont's own, the authority on Icon's syntax
# (docs/design.md, "Two oracles"): for every .icn file of the Icon tree
# beside the checkout (../icon, or $ICON_SRC), the C (src/tests/oracle.c,
# built with the address and undefined-behaviour sanitizers) and icont's
# lexer and parser (tools/icontok, linked from a scratch build of that
# tree) must agree on the token stream, on whether the file is valid,
# and on the tree of every procedure. Then mutants (a token deleted,
# duplicated or swapped) must agree the same way.
#
#   src/tests/oracle.sh [MUTANTS]     (default 300)
#
# Files that use the preprocessor (a directive, or a symbol icont
# predefines) are compared on their "icont -E" expansion; where that has
# #line comments, line numbers and positions are not compared. Files
# icont rejects only for errors that are not syntax (redeclarations,
# unknown keywords) are skipped. VERBOSE=1 shows each difference.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
root=${ICON_SRC:-$here/../icon}
[ -f "$root/src/h/features.h" ] || { echo "oracle.sh: no Icon tree at $root" >&2; exit 2; }
mutants=${1:-300}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
$CC -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$tmp/oracle" "$here"/src/tests/oracle.c "$here"/src/util.c "$here"/src/token.c \
	"$here"/src/lexer.c "$here"/src/lit.c "$here"/src/syntax.c || exit 2
icontok=$("$here/tools/icontok/build.sh" "$root") || exit 2
icont=$(dirname "$icontok")/../../bin/icont
[ -x "$icont" ] || { echo "oracle.sh: no icont beside $icontok" >&2; exit 2; }
C=$tmp/oracle
LPATH="$root/ipl/incl $root/ipl/gincl"
export LPATH
predef=$(sed -n 's/.*Feature([^"]*"\([^"]*\)".*/\1/p' "$root/src/h/features.h")

fail=0
say() { fail=$((fail + 1)); echo "$*"; }
show() { [ "${VERBOSE:-}" ] && diff "$1" "$2" | head -6; }
semantic() { grep -v -e 'inconsistent redeclaration' -e 'redeclared identifier' \
	-e 'invalid keyword' "$1" | grep -q . && return 1; grep -q . "$1"; }
# icont's lines for ";" and end of file are idiosyncratic: not compared.
semis() { awk -F'\t' 'BEGIN { OFS = "\t" } $1 == ";" || $1 == "end-of-file" { $2 = 0 } { print }'; }

# prepare FILE: sets src (the file to compare), lined (#line inside);
# false if icont cannot preprocess it.
prepare() {
	src=$1 lined=
	if $C needspp "$1" $predef; then
		(cd "$(dirname "$1")" && "$icont" -E "$(basename "$1")") >"$tmp/pp.icn" 2>/dev/null || return 1
		src=$tmp/pp.icn
		npp=$((npp + 1))
	fi
	$C linedirs "$src" && lined=1
	return 0
}

# compare FILE LABEL: tokens, then validity and trees
compare() {
	(cd "$(dirname "$1")" && "$icontok" "$src") >"$tmp/it0" 2>/dev/null; iok=$?
	semis <"$tmp/it0" >"$tmp/it"
	$C tokens "$src" >"$tmp/ct" 2>/dev/null; cok=$?
	if [ $iok = 0 ] && [ $cok = 0 ]; then
		if [ "$lined" ]; then
			$C strip tokens <"$tmp/it" >"$tmp/it2"; mv "$tmp/it2" "$tmp/it"
			$C strip tokens <"$tmp/ct" >"$tmp/ct2"; mv "$tmp/ct2" "$tmp/ct"
		fi
		if cmp -s "$tmp/it" "$tmp/ct"; then ntok=$((ntok + 1)); else say "TOKENS $2"; show "$tmp/it" "$tmp/ct"; fi
	elif [ $iok != $cok ]; then
		say "LEXICAL $2: icont $iok, C $cok"
	fi

	(cd "$(dirname "$1")" && "$icontok" -t "$src") >"$tmp/i" 2>"$tmp/ie"; iok=$?
	$C trees "$src" >"$tmp/c" 2>"$tmp/ce"; cok=$?
	if [ $iok != 0 ] && [ $cok != 0 ]; then
		nrej=$((nrej + 1)); return
	fi
	if [ $iok != 0 ] && semantic "$tmp/ie"; then
		nsem=$((nsem + 1)); return
	fi
	if [ $iok != 0 ]; then
		say "ACCEPTS $2: icont: $(head -1 "$tmp/ie")"; return
	fi
	if [ $cok != 0 ]; then
		say "REJECTS $2: C: $(head -1 "$tmp/ce")"; return
	fi
	if [ "$lined" ]; then
		$C strip trees <"$tmp/i" >"$tmp/i2"; mv "$tmp/i2" "$tmp/i"
		$C strip trees <"$tmp/c" >"$tmp/c2"; mv "$tmp/c2" "$tmp/c"
	fi
	if cmp -s "$tmp/i" "$tmp/c"; then
		ntree=$((ntree + 1)); nproc=$((nproc + $(wc -l <"$tmp/c")))
	else
		say "TREES $2"; show "$tmp/i" "$tmp/c"
	fi
}

find "$root" -name '*.icn' -not -path '*/.git/*' | sort >"$tmp/list"
ntok=0 ntree=0 nproc=0 nrej=0 nsem=0 npp=0 nprep=0
while read -r f; do
	prepare "$f" || { nprep=$((nprep + 1)); continue; }
	compare "$f" "${f#$root/}"
done <"$tmp/list"
echo "corpus: tokens identical in $ntok files ($npp preprocessed); trees identical in $ntree files ($nproc procedures); $nrej rejected by both; $nsem with only non-syntax errors; $nprep not preprocessable"

# Mutants: the k-th takes file (k * 7919) mod n of the corpus, seed k.
n=$(wc -l <"$tmp/list")
ntok=0 ntree=0 nproc=0 nrej=0 nsem=0 npp=0 nprep=0 k=0 made=0
while [ $made -lt "$mutants" ] && [ $k -lt $((mutants * 4)) ]; do
	k=$((k + 1))
	f=$(sed -n "$(( (k * 7919) % n + 1 ))p" "$tmp/list")
	prepare "$f" || continue
	$C mutate $k "$src" >"$tmp/m.icn" 2>"$tmp/what" || continue
	made=$((made + 1))
	src=$tmp/m.icn
	compare "$f" "${f#$root/}, $(cat "$tmp/what")"
done
echo "mutants: $made made; trees identical in $ntree; $nrej rejected by both; $nsem with only non-syntax errors"
[ $fail -eq 0 ] && echo "oracle: no differences" || echo "oracle: $fail differences"
[ $fail -eq 0 ]
