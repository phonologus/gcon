#!/bin/sh
# The formatter's test: gcon fmt (built here with the address and
# undefined-behaviour sanitizers) against icfmt, the formatter it is
# ported from (icontools, beside the checkout: ../icontools, or
# $ICONTOOLS; gcon never imports it, it only runs it). For every .icn
# file of the corpus (the Icon tree beside the checkout, gcon's library,
# test data and experiments, ../icn), and damaged variants of them
# (tests/variants.c), the two must write the same output and messages,
# with the same exit status; formatting must keep every file's token
# sequence, inserted semicolons included; and the command's flags must
# behave alike (with icfmt's name read as gcon fmt's).
#
#   src/tests/fmt.sh
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
tools=${ICONTOOLS:-$here/../icontools}
[ -d "$tools/cmd/icfmt" ] || { echo "fmt.sh: no icontools at $tools" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
(cd "$tools" && go build -o "$tmp/icfmt" ./cmd/icfmt) || exit 2
$CC -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-DGCON_VERSION='"test"' -I"$here/vm/src" -o "$tmp/gcon" "$here"/src/*.c "$here/vm/src/gen_tables.c" || exit 2
$CC -std=c11 -O1 -I"$here/vm/src" -o "$tmp/dump" "$here"/src/tests/dump.c "$here"/src/util.c \
	"$here"/src/token.c "$here"/src/lexer.c "$here"/src/lit.c "$here"/src/syntax.c "$here"/src/ipp.c \
	"$here"/src/builtin.c "$here"/src/sema.c "$here"/src/prog.c "$here"/src/compile.c \
	"$here/vm/src/gen_tables.c" || exit 2
$CC -std=c11 -O1 -o "$tmp/variants" "$here/src/tests/variants.c" || exit 2
printf '#!/bin/sh\nexec "%s" fmt "$@"\n' "$tmp/gcon" >"$tmp/gfmt"
chmod +x "$tmp/gfmt"
LC_ALL=C
export LC_ALL

{
	[ -d "$here/../icon" ] && find "$here/../icon" -name '*.icn' -not -path '*/.git/*'
	find "$here/lib" "$here/experiments" "$here/vm/tests" "$here/src/tests" -name '*.icn'
	[ -d "$here/../icn" ] && find "$here/../icn" -name '*.icn'
} | sort >"$tmp/list"
mkdir "$tmp/var"
"$tmp/variants" "$tmp/var" <"$tmp/list"
find "$tmp/var" -name '*.icn' | sort >>"$tmp/list"

# toks: a file's tokens, as kind, implicit or not, and text
toks() {
	"$tmp/dump" lex "$1" | awk '/^T / { k = $2; imp = ($5 == "implicit") } /^  text / { sub(/^  text /, ""); print k, imp, $0 }'
}

fail=0 same=0 kept=0 changed=0
while read -r f; do
	"$tmp/icfmt" "$f" >"$tmp/i.out" 2>"$tmp/i.err"; is=$?
	"$tmp/gfmt" "$f" >"$tmp/c.out" 2>"$tmp/c.err"; cs=$?
	if cmp -s "$tmp/i.out" "$tmp/c.out" && cmp -s "$tmp/i.err" "$tmp/c.err" && [ $is = $cs ]; then
		same=$((same + 1))
	else
		fail=$((fail + 1))
		echo "DIFF $f (status $is, $cs)"
	fi
	if [ $cs = 0 ]; then
		cmp -s "$f" "$tmp/c.out" || changed=$((changed + 1))
		toks "$f" >"$tmp/t1"
		toks "$tmp/c.out" >"$tmp/t2"
		if cmp -s "$tmp/t1" "$tmp/t2"; then kept=$((kept + 1)); else fail=$((fail + 1)); echo "TOKENS $f"; fi
	fi
done <"$tmp/list"
echo "fmt: $same files identical to icfmt ($changed changed by formatting); tokens kept in $kept"

# The command's flags, on a small tree.
mkdir -p "$tmp/cli/a/sub"
cp "$here/src/tests/pp/expand.icn" "$tmp/cli/a/"
cp "$here/vm/tests/programs/arith.icn" "$tmp/cli/a/sub/"
printf 'procedure main(\nend\n' >"$tmp/cli/a/broken.icn"
touch "$tmp/cli/a/notes.txt"
cli() { # label, command with $I as the formatter
	label=$1; shift
	(cd "$tmp/cli" && I="$tmp/icfmt" && eval "$*") >"$tmp/i.out" 2>&1; echo "status $?" >>"$tmp/i.out"
	(cd "$tmp/cli" && I="$tmp/gfmt" && eval "$*") >"$tmp/c.out" 2>&1; echo "status $?" >>"$tmp/c.out"
	sed 's/gcon fmt/icfmt/g' "$tmp/c.out" >"$tmp/c2.out"
	if cmp -s "$tmp/i.out" "$tmp/c2.out"; then :; else fail=$((fail + 1)); echo "CLI $label"; diff "$tmp/i.out" "$tmp/c2.out" | head -6; fi
}
cli "-l" '$I -l a'
cli "-d" '$I -d a/sub/arith.icn'
cli "stdin" '$I <a/sub/arith.icn'
cli "-w with stdin" '$I -w'
cli "missing" '$I nosuch.icn'
cli "broken" '$I a/broken.icn'
cli "flag" '$I -x a'
cli "-w" 'rm -rf w && cp -R a w && $I -w w; cat w/sub/arith.icn w/expand.icn'
[ $fail -eq 0 ] && echo "fmt: no differences" || echo "fmt: $fail differences"
[ $fail -eq 0 ]
