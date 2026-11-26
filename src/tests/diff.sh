#!/bin/sh
# The differential test of the C front end against the Go reference: the
# C (src/tests/dump.c, built with the address and undefined-behaviour
# sanitizers) and the Go (tools/refdump) write the same canonical form of
# each file, and the two must be identical, file for file.
#
#   src/tests/diff.sh [lex|parse|walk|pp|sema] [file ...]
#
# The files are, by default, the corpus: every .icn file of the Icon tree
# beside the checkout (../icon), of gcon's library, test data and
# experiments, and of ../icn; then variants of them made with a fixed
# seed (truncations, changed bytes) and files of random bytes, which
# reach the error paths. VERBOSE=n shows the first n lines of each
# difference.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
what=lex
case "${1:-}" in lex|parse|walk|pp|sema) what=$1; shift ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
$CC -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$tmp/dump" "$here"/src/tests/dump.c "$here"/src/util.c "$here"/src/token.c \
	"$here"/src/lexer.c "$here"/src/lit.c "$here"/src/syntax.c "$here"/src/ipp.c \
	"$here"/src/builtin.c "$here"/src/sema.c || exit 2
(cd "$here" && go build -o "$tmp/refdump" ./tools/refdump) || exit 2

if [ $# -gt 0 ]; then
	for f; do echo "$f"; done >"$tmp/list"
else
	{
		[ -d "$here/../icon" ] && find "$here/../icon" -name '*.icn' -not -path '*/.git/*'
		find "$here/lib" "$here/internal" "$here/experiments" "$here/vm/tests" "$here/src/tests" -name '*.icn'
		[ -d "$here/../icn" ] && find "$here/../icn" -name '*.icn'
	} | sort >"$tmp/list"
	# Variants, deterministic (tests/variants.c): every 7th file
	# truncated at three points and with ten bytes changed; and 50 files
	# of random bytes.
	mkdir "$tmp/var"
	$CC -std=c11 -O1 -Wall -Wextra -o "$tmp/variants" "$here"/src/tests/variants.c || exit 2
	"$tmp/variants" "$tmp/var" <"$tmp/list" || exit 2
	find "$tmp/var" -name '*.icn' | sort >>"$tmp/list"
fi

# pp: each file is preprocessed in its own directory, by its own name,
# with the predefined symbols of the Go, and the IPL's include
# directories on the path.
if [ $what = pp ] || [ $what = sema ]; then
	PPDEFS=$("$tmp/refdump" predef)
	PPPATH="$here/../icon/ipl/incl $here/../icon/ipl/gincl"
	export PPDEFS PPPATH
fi

pass=0 fail=0
while read -r f; do
	if [ $what = pp ] || [ $what = sema ]; then
		(cd "$(dirname "$f")" && "$tmp/refdump" $what "$(basename "$f")") >"$tmp/go" 2>&1
		(cd "$(dirname "$f")" && "$tmp/dump" $what "$(basename "$f")") >"$tmp/c" 2>&1
	else
		"$tmp/refdump" "$what" "$f" >"$tmp/go" 2>&1
		"$tmp/dump" "$what" "$f" >"$tmp/c" 2>&1
	fi
	if cmp -s "$tmp/go" "$tmp/c"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "DIFF $f"
		[ "${VERBOSE:-}" ] && diff "$tmp/go" "$tmp/c" | head -"$VERBOSE"
	fi
done <"$tmp/list"
echo "$what: $pass identical, $fail differ"
[ $fail -eq 0 ]
