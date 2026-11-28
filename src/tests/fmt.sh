#!/bin/sh
# The formatter's test: gcon fmt (built here with the address and
# undefined-behaviour sanitizers) is its own oracle.
#
#   - Golden: for the repository's own Icon sources (src/tests,
#     vm/tests, experiments), the output, messages and exit status
#     recorded in src/tests/fmt/PATH.golden; and the command's flags, in
#     src/tests/fmt/cli.golden.
#   - For every .icn file of the corpus (the Icon tree beside the
#     checkout, gcon's library and tests, ../icn) and damaged variants
#     of them (tests/variants.c): no crash (exit status 0, 1 or 2, nothing
#     from the sanitizers); a formatted file keeps its token sequence,
#     inserted semicolons included; and formatting it again changes
#     nothing.
#
#   src/tests/fmt.sh            check
#   src/tests/fmt.sh -update    rewrite the golden files
#
# JOBS workers share the corpus (default: one per CPU).
set -u
# gcon exits without freeing what it built, as icont does: no leak checks
# (ASan makes them on Linux, by default)
export ASAN_OPTIONS=detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}
here=$(cd "$(dirname "$0")/../.." && pwd)
gold=$here/src/tests/fmt
update=
[ "${1:-}" = -update ] && update=1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
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
fail=0

# Golden files.
(cd "$here" && find src/tests vm/tests experiments -name '*.icn') | sort >"$tmp/golden"
gfail=0 gn=0
while read -r rel; do
	gn=$((gn + 1))
	(cd "$here" && "$tmp/gfmt" "$rel") >"$tmp/g.out" 2>&1
	echo "status $?" >>"$tmp/g.out"
	want=$gold/$rel.golden
	if [ "$update" ]; then
		mkdir -p "$(dirname "$want")"
		cp "$tmp/g.out" "$want"
	elif ! cmp -s "$want" "$tmp/g.out"; then
		gfail=$((gfail + 1))
		echo "GOLDEN $rel"
		diff "$want" "$tmp/g.out" 2>&1 | head -6
	fi
done <"$tmp/golden"

# The command's flags, on a small tree.
mkdir -p "$tmp/cli/a/sub"
cp "$here/src/tests/pp/expand.icn" "$tmp/cli/a/"
cp "$here/vm/tests/programs/arith.icn" "$tmp/cli/a/sub/"
printf 'procedure main(\nend\n' >"$tmp/cli/a/broken.icn"
touch "$tmp/cli/a/notes.txt"
cli() { # label, command with $I as the formatter
	echo "== $1"
	(cd "$tmp/cli" && I="$tmp/gfmt" && eval "$2") 2>&1
	echo "status $?"
}
{
	cli "-l" '$I -l a'
	cli "-d" '$I -d a/sub/arith.icn'
	cli "stdin" '$I <a/sub/arith.icn'
	cli "-w with stdin" '$I -w'
	cli "missing" '$I nosuch.icn'
	cli "broken" '$I a/broken.icn'
	cli "flag" '$I -x a'
	cli "-w" 'rm -rf w && cp -R a w && $I -w w; cat w/sub/arith.icn w/expand.icn'
} >"$tmp/cli.out"
if [ "$update" ]; then
	cp "$tmp/cli.out" "$gold/cli.golden"
elif ! cmp -s "$gold/cli.golden" "$tmp/cli.out"; then
	gfail=$((gfail + 1))
	echo "GOLDEN cli.golden"
	diff "$gold/cli.golden" "$tmp/cli.out" | head -10
fi
if [ "$update" ]; then
	echo "fmt: $gn golden files and cli.golden rewritten"
else
	echo "fmt: $gn golden files and cli.golden, $gfail differ"
fi
fail=$((fail + gfail))

# The corpus and its variants.
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

# work LIST OUT: check LIST's files, writing a line for each to OUT:
# "ok", "changed", "error" (rejected, exit 1 or 2), or a problem.
work() {
	w=$2.tmp
	while read -r f; do
		"$tmp/gfmt" "$f" >"$w.out" 2>"$w.err"
		s=$?
		if grep -Eq "Sanitizer|runtime error" "$w.err" || [ $s -gt 2 ]; then
			echo "CRASH $f (status $s)"
			continue
		fi
		if [ $s != 0 ]; then
			echo error
			continue
		fi
		toks "$f" >"$w.t1"
		toks "$w.out" >"$w.t2"
		if ! cmp -s "$w.t1" "$w.t2"; then
			echo "TOKENS $f"
			continue
		fi
		"$tmp/gfmt" "$w.out" >"$w.out2" 2>/dev/null
		if ! cmp -s "$w.out" "$w.out2"; then
			echo "IDEMPOTENT $f"
			continue
		fi
		if cmp -s "$f" "$w.out"; then echo ok; else echo changed; fi
	done <"$1" >"$2"
}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
awk -v n="$jobs" -v d="$tmp" '{ print > (d "/part" (NR % n)) }' "$tmp/list"
for part in "$tmp"/part*; do
	work "$part" "$part.res" &
done
wait
cat "$tmp"/part*.res >"$tmp/results"
grep -Ev '^(ok|changed|error)$' "$tmp/results"
n=$(wc -l <"$tmp/list" | tr -d ' ')
problems=$(grep -Evc '^(ok|changed|error)$' "$tmp/results")
echo "fmt: $n corpus files and variants: $(grep -c '^changed$' "$tmp/results") changed by formatting," \
	"$(grep -c '^ok$' "$tmp/results") already formatted, $(grep -c '^error$' "$tmp/results") rejected; $problems problems"
fail=$((fail + problems))
[ $fail -eq 0 ]
