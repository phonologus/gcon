#!/bin/sh
# gcon help's test (gcon built here with the address and
# undefined-behaviour sanitizers). The pages in src/help are checked
# against gcon and against the code:
#
#   - src/helppages.inc, docstyle.inc and docsearch.inc are what
#     tools/embed.sh makes of src/help and src/doc.
#   - "gcon help NAME" prints NAME.txt, and "gcon help" the overview, on
#     standard output with exit status 0; "gcon CMD -h" and "--help" print
#     a command's page the same way.
#   - Misuse of a command prints its usage (its page up to the first
#     blank line) and a pointer to the page on standard error, with exit
#     status 2; so do an unknown command (the pointer only) and no
#     command at all (the overview).
#   - The overview lists exactly the pages; the commands main.c
#     dispatches are exactly the pages with a usage.
#   - Every flag in a command's usage has an entry in its page's flag
#     list (a line "<tab>-flag").
#   - Every environment variable the C reads (getenv in src, vm/src and
#     graphics/src) is in gcon help environment, in
#     design/development.md's list for development, or a system's own.
#
#   src/tests/help.sh
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
pages=$here/src/help
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
$CC -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-DGCON_VERSION='"test"' -I"$here/vm/src" -o "$tmp/gcon" "$here"/src/*.c "$here/vm/src/gen_tables.c" || exit 2
GCONROOT=$here
LC_ALL=C
export GCONROOT LC_ALL
fail=0
checks=0

problem() {
	echo "FAIL $*"
	fail=$((fail + 1))
}

# expect STATUS OUTFILE ERRFILE -- ARGS: gcon ARGS exits with STATUS,
# writing exactly OUTFILE on standard output and ERRFILE on standard
# error (/dev/null for nothing).
expect() {
	want=$1 wout=$2 werr=$3
	shift 4
	checks=$((checks + 1))
	"$tmp/gcon" "$@" >"$tmp/out" 2>"$tmp/err" </dev/null
	st=$?
	[ "$st" = "$want" ] || problem "gcon $*: exit status $st, want $want"
	cmp -s "$tmp/out" "$wout" || problem "gcon $*: standard output differs from $wout"
	cmp -s "$tmp/err" "$werr" || {
		problem "gcon $*: standard error differs"
		sed 's/^/	/' "$tmp/err" | head -5
	}
}
# The embedded files are current.
mkdir "$tmp/embed"
sh "$here/tools/embed.sh" "$tmp/embed" || problem "tools/embed.sh failed"
for f in helppages docstyle docsearch; do
	checks=$((checks + 1))
	cmp -s "$tmp/embed/$f.inc" "$here/src/$f.inc" || problem "src/$f.inc is not current: run tools/embed.sh"
done

# Every page, asked for.
expect 0 "$pages/gcon.txt" /dev/null -- help
expect 0 "$pages/help.txt" /dev/null -- help -h
names=
commands=
for f in "$pages"/*.txt; do
	n=$(basename "$f" .txt)
	[ "$n" = gcon ] && continue
	names="$names $n"
	expect 0 "$f" /dev/null -- help "$n"
	if head -1 "$f" | grep -q "^usage: gcon $n\\b"; then
		commands="$commands $n"
		if [ "$n" != help ]; then
			expect 0 "$f" /dev/null -- "$n" -h
			expect 0 "$f" /dev/null -- "$n" --help
		fi
	fi
done

# usage NAME: the usage gcon prints for a misused command NAME.
usage() {
	awk '/^$/ { exit } { print }' "$pages/$1.txt"
	echo "Run 'gcon help $1' for details."
}
for misuse in "run" "dis" "dis a b" "build" "build -x f.icn" "build -o" "build a.icn b.icn" \
	"vet" "vet -x f.icn" "fmt -x" "lsp -x" "lsp fetch-docs -x" "env x" "version x" "doc -x" "doc a b"; do
	set -- $misuse
	usage "$1" >"$tmp/usage"
	case "$misuse" in
	*" -x"*) { echo "flag provided but not defined: -x"; cat "$tmp/usage"; } >"$tmp/want" ;;
	"build -o") { echo "flag needs an argument: -o"; cat "$tmp/usage"; } >"$tmp/want" ;;
	*) cp "$tmp/usage" "$tmp/want" ;;
	esac
	# lsp's own parser reports a bad flag by its usage alone
	case "$misuse" in "lsp -x" | "lsp fetch-docs -x") cp "$tmp/usage" "$tmp/want" ;; esac
	expect 2 /dev/null "$tmp/want" -- $misuse
done
printf "gcon nosuch: unknown command\nRun 'gcon help' for usage.\n" >"$tmp/want"
expect 2 /dev/null "$tmp/want" -- nosuch
printf "gcon help nosuch: unknown help topic. Run 'gcon help'.\n" >"$tmp/want"
expect 2 /dev/null "$tmp/want" -- help nosuch
expect 2 /dev/null "$pages/gcon.txt" --

# The overview lists the pages; main.c dispatches the commands.
checks=$((checks + 2))
listed=$(awk '/^\t[a-z0-9]+  / { print $1 }' "$pages/gcon.txt" | sort | tr '\n' ' ')
have=$(echo $names | tr ' ' '\n' | sort | tr '\n' ' ')
[ "$listed" = "$have" ] || problem "the overview lists [$listed], the pages are [$have]"
dispatched=$(grep -o '!strcmp(cmd, "[a-z]*")' "$here/src/main.c" | sed 's/.*"\(.*\)".*/\1/' | sort -u | tr '\n' ' ')
cmds=$(echo $commands | tr ' ' '\n' | sort | tr '\n' ' ')
[ "$dispatched" = "$cmds" ] || problem "main.c dispatches [$dispatched], the command pages are [$cmds]"

# Every flag in a usage has an entry.
for n in $commands; do
	for flag in $(awk '/^$/ { exit } { print }' "$pages/$n.txt" | grep -o '\[-[a-z0-9]*' | sed 's/^\[//'); do
		checks=$((checks + 1))
		grep -q "^	$flag\\b" "$pages/$n.txt" || problem "gcon help $n: $flag is in the usage but has no entry"
	done
done

# Every environment variable is documented.
for v in $(cat "$here"/src/*.c "$here"/src/*.h "$here"/vm/src/*.c "$here"/vm/src/*.h \
	"$here"/graphics/src/*.c "$here"/graphics/src/*.h 2>/dev/null |
	grep -o 'getenv("[A-Za-z_][A-Za-z0-9_]*")' | sed 's/getenv("\(.*\)")/\1/' | sort -u); do
	checks=$((checks + 1))
	case $v in HOME | PATH | TMPDIR | XDG_CACHE_HOME | DISPLAY | WAYLAND_DISPLAY) continue ;; esac
	grep -q "^	$v\$" "$pages/environment.txt" && continue
	grep -q "^| \`$v[=\`]" "$here/design/development.md" && continue
	problem "$v is read but documented in neither gcon help environment nor design/development.md"
done

echo "help: $(echo $names | wc -w | tr -d ' ') pages, $checks checks; $fail problems"
[ "$fail" = 0 ]
