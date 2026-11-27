#!/bin/sh
# gcon doc's test (gcon built here with the address and
# undefined-behaviour sanitizers):
#
#   - Golden: src/tests/doc/probe.md, every construct of the
#     documentation's Markdown, as HTML, text and roff (PROBE.html,
#     .txt, .man); errors.md's errors, one of each kind (errors.golden).
#   - The tree: gcon doc -check passes (the subset, the links and
#     anchors); -build writes a page for every topic, the stylesheet,
#     the script and the search index; with mandoc, every page's roff
#     lints clean (but for the version in .TH's date).
#   - The internals and the code: a row for every instruction in
#     doc/internals/instructions.md; format.h's version in program.md;
#     every graphics request, with its number, in protocol.md.
#   - Lookup: a topic, its last element, a file; an unknown name.
#   - The browser: $BROWSER is given the page's file URL; a browser
#     that fails, or no HTML built, gives the page as text.
#
#   src/tests/doc.sh            check
#   src/tests/doc.sh -update    rewrite the golden files
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
probes=$here/src/tests/doc
update=
[ "${1:-}" = -update ] && update=1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CC=${CC:-cc}
$CC -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	-DGCON_VERSION='"test"' -I"$here/vm/src" -o "$tmp/gcon" "$here"/src/*.c "$here/vm/src/gen_tables.c" || exit 2
G=$tmp/gcon
LC_ALL=C
export LC_ALL
unset BROWSER PAGER
fail=0
checks=0

problem() {
	echo "FAIL $*"
	fail=$((fail + 1))
}

# golden NAME FILE: compare FILE with NAME (or record it).
golden() {
	checks=$((checks + 1))
	if [ "$update" ]; then
		cp "$2" "$probes/$1"
	elif ! cmp -s "$2" "$probes/$1"; then
		problem "golden $1"
		diff "$probes/$1" "$2" | head -20
	fi
}

# The renderers, and the errors.
GCONROOT=$here
export GCONROOT
"$G" doc -html "$probes/probe.md" >"$tmp/out" 2>&1 || problem "probe.md: -html failed"
golden probe.html "$tmp/out"
"$G" doc -text "$probes/probe.md" >"$tmp/out" 2>&1 || problem "probe.md: -text failed"
golden probe.txt "$tmp/out"
"$G" doc -man "$probes/probe.md" >"$tmp/out" 2>&1 || problem "probe.md: -man failed"
golden probe.man "$tmp/out"
checks=$((checks + 1))
"$G" doc -raw "$probes/probe.md" | cmp -s - "$probes/probe.md" || problem "probe.md: -raw is not the file"
(cd "$probes" && "$G" doc -text errors.md >/dev/null 2>"$tmp/out"; echo "status $?" >>"$tmp/out")
golden errors.golden "$tmp/out"

# The tree.
checks=$((checks + 1))
"$G" doc -check 2>"$tmp/err" || {
	problem "gcon doc -check"
	head -20 "$tmp/err"
}
checks=$((checks + 1))
"$G" doc -build -o "$tmp/html" 2>"$tmp/err" || problem "gcon doc -build: $(head -3 "$tmp/err")"
"$G" doc -list | awk '{ print $1 }' >"$tmp/topics"
while read -r topic; do
	checks=$((checks + 1))
	[ -f "$tmp/html/$topic.html" ] || problem "-build wrote no $topic.html"
	grep -q "\"$topic.html" "$tmp/html/search-index.js" || problem "the search index has no $topic"
done <"$tmp/topics"
for f in style.css search.js; do
	checks=$((checks + 1))
	cmp -s "$tmp/html/$f" "$here/src/doc/$f" || problem "-build's $f is not src/doc/$f"
done
if command -v mandoc >/dev/null 2>&1; then
	while read -r topic; do
		checks=$((checks + 1))
		"$G" doc -man "$topic" >"$tmp/page.7"
		mandoc -T lint -W warning "$tmp/page.7" 2>&1 | grep -v 'cannot parse date' >"$tmp/lint"
		[ -s "$tmp/lint" ] && {
			problem "mandoc lint of $topic"
			head -5 "$tmp/lint"
		}
	done <"$tmp/topics"
fi

# The internals pages and the code: every instruction has a row in the
# table of doc/internals/instructions.md, and doc/internals/program.md
# gives format.h's version.
ops=$(awk '/opnames\[NumOps\] = \{/ { on = 1; next } on && /\};/ { exit } on' "$here/vm/src/dis.c" | grep -o '"[a-z]*"' | tr -d '"')
for op in $ops; do
	checks=$((checks + 1))
	grep -q "^| \`$op\` |" "$here/doc/internals/instructions.md" || problem "instruction $op has no row in doc/internals/instructions.md"
done
rows=$(grep -c '^| `[a-z]*` | `' "$here/doc/internals/instructions.md")
checks=$((checks + 1))
[ "$rows" = "$(echo $ops | wc -w | tr -d ' ')" ] || problem "doc/internals/instructions.md has $rows instruction rows for $(echo $ops | wc -w | tr -d ' ') instructions"
fv=$(sed -n 's/^enum { FormatVersion = \([0-9]*\) };$/\1/p' "$here/vm/src/format.h")
checks=$((checks + 1))
grep -q "format version $fv:" "$here/doc/internals/program.md" && grep -q "a varint: $fv\\." "$here/doc/internals/program.md" ||
	problem "doc/internals/program.md does not give format version $fv"

# doc/internals/protocol.md gives every request of vm/src/gfx.h's enum
# its number.
i=0
for op in $(awk '/P_HELLO,/ { on = 1 } on { print } on && /};/ { exit }' "$here/vm/src/gfx.h" | grep -o 'P_[A-Z]*' | sed 's/^P_//'); do
	checks=$((checks + 1))
	grep -q "\`$op\` ($i)" "$here/doc/internals/protocol.md" || problem "doc/internals/protocol.md does not give request $op as ($i)"
	i=$((i + 1))
done

# Lookup.
want=$here/doc/html/help/run.html
for name in help/run run help/run.md; do
	checks=$((checks + 1))
	got=$("$G" doc -path "$name")
	[ "$got" = "$want" ] || problem "gcon doc -path $name: $got, want $want"
done
checks=$((checks + 1))
"$G" doc -path nosuch >/dev/null 2>"$tmp/err"
[ $? = 2 ] && grep -q 'no such page' "$tmp/err" || problem "gcon doc nosuch: not refused"

# Reference entries: found by name, shown alone, opened at their anchor.
checks=$((checks + 3))
[ "$("$G" doc -path upto)" = "$here/doc/html/ref/scanning.html" ] || problem "gcon doc -path upto: not ref/scanning.html"
"$G" doc -raw '&pos' | head -1 | grep -qx '## &pos' || problem "gcon doc -raw &pos: not the entry"
"$G" doc -text find | grep -q '^See also' && ! "$G" doc -text find | grep -q '^many$' || problem "gcon doc -text find: not the entry alone"

# The examples: each with an output gives it; each without translates
# (gcon run needs the root's gcon-vm, which build.sh builds).
if [ -x "$here/libexec/gcon-vm" ]; then
	checks=$((checks + 1))
	"$G" doc -test >"$tmp/out" 2>&1 || {
		problem "gcon doc -test"
		head -20 "$tmp/out"
	}
else
	echo "doc: no libexec/gcon-vm (run build.sh): examples not run"
fi

# The browser, in a root of its own, its HTML built.
root=$tmp/root
mkdir -p "$root/lib/ipl/procs"
cp -R "$here/doc" "$root/doc"
rm -rf "$root/doc/html"
GCONROOT=$root
"$G" doc -text help/run >"$tmp/text"
checks=$((checks + 1))
"$G" doc help/run >"$tmp/out" 2>"$tmp/err"
cmp -s "$tmp/out" "$tmp/text" && grep -q 'not built' "$tmp/err" || problem "no HTML: not the page as text, with a note"
"$G" doc -build || problem "gcon doc -build in the root"
printf '#!/bin/sh\necho "$1" >"%s"\n' "$tmp/url" >"$tmp/browser"
chmod +x "$tmp/browser"
checks=$((checks + 1))
BROWSER=$tmp/browser "$G" doc run >"$tmp/out" 2>&1
[ "$(cat "$tmp/url" 2>/dev/null)" = "file://$root/doc/html/help/run.html" ] && [ ! -s "$tmp/out" ] ||
	problem "BROWSER was not given help/run's URL: $(cat "$tmp/url" 2>/dev/null) $(cat "$tmp/out")"
checks=$((checks + 1))
BROWSER=$tmp/browser "$G" doc upto >"$tmp/out" 2>&1
[ "$(cat "$tmp/url" 2>/dev/null)" = "file://$root/doc/html/ref/scanning.html#upto" ] ||
	problem "BROWSER was not given upto's URL, at its anchor: $(cat "$tmp/url" 2>/dev/null)"
BROWSER=false "$G" doc run >"$tmp/out" 2>"$tmp/err"
cmp -s "$tmp/out" "$tmp/text" && grep -q 'no browser' "$tmp/err" || problem "a failing browser: not the page as text, with a note"

echo "doc: $checks checks; $fail problems"
[ "$fail" = 0 ]
