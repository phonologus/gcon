#!/bin/sh
# gcon doc's test (gcon built here with the address and
# undefined-behaviour sanitizers):
#
#   - Golden: src/tests/doc/probe.md, every construct of the
#     documentation's Markdown, as HTML, text and roff (PROBE.html,
#     .txt, .man); errors.md's errors, one of each kind (errors.golden).
#   - Doc comments: golden files of comments.icn's, as Markdown and
#     text; the corpus's Icon files all give Markdown in the subset.
#   - The tree: gcon doc -check passes (the subset, the links and
#     anchors); -build writes a page for every topic, the stylesheet,
#     the script and the search index; with mandoc, every page's roff
#     lints clean (but for the version in .TH's date).
#   - The internals and the code: a row for every instruction in
#     doc/internals/instructions.md; format.h's version in program.md;
#     every graphics request, with its number, in protocol.md.
#   - The reference: an entry for every built-in function and keyword of
#     src/builtin.inc (but those not provided), and no others.
#   - Lookup: a topic, its last element, a file; an unknown name.
#   - The IPL: gcon doc -text ipl, a file and a procedure from their header
#     comments, the candidates marked; every candidate a file of lib/ipl.
#   - The server: $BROWSER is given the served page's URL; its pages,
#     the IPL's, a run, Stop, the guards on running, stopping when
#     the browser's pages have gone (not with -http); a browser
#     that fails gives the page as text.
#
#   src/tests/doc.sh            check
#   src/tests/doc.sh -update    rewrite the golden files
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
probes=$here/src/tests/doc
update=
[ "${1:-}" = -update ] && update=1
tmp=$(mktemp -d)
server=
trap '[ -n "$server" ] && kill $server 2>/dev/null; rm -rf "$tmp"' EXIT
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

# Doc comments: src/tests/doc/comments.icn as Markdown (comments.md) and
# as text (comments.txt); one declaration alone; and the Markdown made of
# every Icon file of the corpus (the Icon tree beside the checkout, ../icn)
# within the documentation's subset.
(cd "$probes" && "$G" doc -raw comments.icn >"$tmp/out" 2>&1)
golden comments.md "$tmp/out"
(cd "$probes" && "$G" doc -text comments.icn >"$tmp/out" 2>&1)
golden comments.txt "$tmp/out"
checks=$((checks + 2))
(cd "$probes" && "$G" doc -raw comments.icn total) | head -1 | grep -qx '# total' || problem "gcon doc comments.icn total: not the procedure alone"
(cd "$probes" && "$G" doc comments.icn nosuch >/dev/null 2>&1; [ $? = 2 ]) || problem "gcon doc comments.icn nosuch: not refused"
cn=0 cbad=0
for f in $(find "$here/../icon/ipl" "$here/../icon/tests" "$here/../icn" -name '*.icn' 2>/dev/null); do
	cn=$((cn + 1))
	"$G" doc -raw "$f" >"$tmp/c.md" 2>"$tmp/c.err" && "$G" doc -text "$tmp/c.md" >/dev/null 2>>"$tmp/c.err"
	if [ -s "$tmp/c.err" ]; then
		cbad=$((cbad + 1))
		[ $cbad -le 5 ] && { echo "doc comments of $f:"; head -3 "$tmp/c.err"; }
	fi
done
checks=$((checks + 1))
[ $cbad = 0 ] || problem "doc comments: $cbad of $cn corpus files give Markdown outside the subset"

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

# doc/ref/uclass.md is what tools/uclass.sh makes of the Unicode data.
mkdir "$tmp/uclass"
checks=$((checks + 1))
sh "$here/tools/uclass.sh" -o "$tmp/uclass" >/dev/null &&
	cmp -s "$tmp/uclass/uclass.md" "$here/doc/ref/uclass.md" ||
	problem "doc/ref/uclass.md is not current: run tools/uclass.sh"

# The reference is complete: an entry for every function and keyword of
# src/builtin.inc but those not provided (fattrib, the Win functions), and
# for every documented procedure ("#:") of Gcon's own library, lib/gcon;
# no other entries, and none twice.
awk -F'"' '/^static const BuiltinFunc functions/ { f = 1 } /^static const Keyword keywords/ { f = 0; k = 1 }
	/^static const Predefined/ { k = 0 } f && /^\t\{"/ { print $2 } k && /^\t\{"/ { print "&" $2 }' "$here/src/builtin.inc" |
	grep -v '^Win\|^fattrib$' >"$tmp/want.0"
for f in "$tmp/uclass/uclass.icn" "$here"/lib/gcon/*.icn; do
	[ "$(basename "$f")" = uclass.icn ] && [ "$f" != "$tmp/uclass/uclass.icn" ] && continue
	[ -f "$f" ] && sed -n 's/^procedure \([A-Za-z_][A-Za-z0-9_]*\)(.*#:.*/\1/p' "$f"
done >>"$tmp/want.0"
sort "$tmp/want.0" >"$tmp/want"
grep -h '^## ' "$here"/doc/ref/*.md | sed 's/^## //' | grep -v ' ' | sort >"$tmp/have"
checks=$((checks + 3))
missing=$(comm -23 "$tmp/want" "$tmp/have" | tr '\n' ' ')
extra=$(comm -13 "$tmp/want" "$tmp/have" | tr '\n' ' ')
dups=$(uniq -d "$tmp/have" | tr '\n' ' ')
[ -z "$missing" ] || problem "the reference has no entry for: $missing"
[ -z "$extra" ] || problem "the reference has entries for what is not built in: $extra"
[ -z "$dups" ] || problem "the reference has more than one entry for: $dups"

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

# The IPL: shown apart from the tree, from its header comments; the
# candidates for the standard library marked; every candidate a file.
checks=$((checks + 6))
"$G" doc -text ipl | grep -q '^procs/strings  *candidate  *Procedures for manipulating strings' ||
	problem "gcon doc ipl: procs/strings not listed as a candidate"
"$G" doc -text WOpen | grep -q "candidate for Gcon's standard library" && "$G" doc -text WOpen | grep -q 'ipl/gprocs/wopen' ||
	problem "gcon doc WOpen: not wopen.icn's procedure, marked as a candidate"
"$G" doc -text ipl/bitint | grep -q 'From the Icon Program Library' && ! "$G" doc -text ipl/bitint | grep -q 'candidate' ||
	problem "gcon doc ipl/bitint: not the IPL's file, unmarked"
"$G" doc -text graphics | head -1 | grep -qx 'Graphics' || problem "gcon doc graphics: not the guide's page"
"$G" doc -check 2>&1 | grep -q 'ipl' && problem "gcon doc -check looks at the IPL"
bad=$(grep -v '^#' "$here/doc/ipl-candidates.txt" | while read -r f; do [ -f "$here/lib/ipl/$f.icn" ] || echo "$f"; done)
[ -z "$bad" ] || problem "doc/ipl-candidates.txt names files that are not in lib/ipl: $bad"

# The browser and the server, in a root of its own: gcon doc serves the
# tree on the loopback address and gives $BROWSER the page's URL; a
# browser that fails gives the page as text. With curl: the pages, the
# IPL's, a run (with gcon-vm), and the guards on running.
root=$tmp/root
mkdir -p "$root/lib/ipl/procs" "$root/lib/ipl/gprocs" "$tmp/t"
cp "$here/lib/ipl/gprocs/wopen.icn" "$root/lib/ipl/gprocs/"
cp -R "$here/doc" "$root/doc"
rm -rf "$root/doc/html"
GCONROOT=$root
export GCONROOT
"$G" doc -text help/run >"$tmp/text"
printf '#!/bin/sh\necho "$1" >"%s.new" && mv "%s.new" "%s"\n' "$tmp/url" "$tmp/url" "$tmp/url" >"$tmp/browser"
chmod +x "$tmp/browser"
# serve NAME: start gcon doc NAME, wait for the browser's URL
serve() {
	rm -f "$tmp/url"
	TMPDIR=$tmp/t BROWSER=$tmp/browser "$G" doc "$1" >"$tmp/out" 2>"$tmp/err" &
	server=$!
	n=0
	while [ ! -s "$tmp/url" ] && [ $n -lt 100 ] && kill -0 $server 2>/dev/null; do
		sleep 0.1
		n=$((n + 1))
	done
	url=$(cat "$tmp/url" 2>/dev/null)
	base=$(echo "$url" | sed -n 's|^\(http://127\.0\.0\.1:[0-9]*\)/.*|\1|p')
}
stop() { kill $server 2>/dev/null; wait $server 2>/dev/null; }
checks=$((checks + 2))
serve run
echo "$url" | grep -q '^http://127\.0\.0\.1:[0-9]*/help/run\.html$' || problem "BROWSER was not given help/run's URL: $url $(cat "$tmp/err")"
stop
serve upto
echo "$url" | grep -q '^http://127\.0\.0\.1:[0-9]*/ref/scanning\.html#upto$' || problem "BROWSER was not given upto's URL, at its anchor: $url"
if command -v curl >/dev/null 2>&1 && [ -n "$base" ]; then
	code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
	checks=$((checks + 6))
	[ "$(code "$base/ref/scanning.html")" = 200 ] && [ "$(code "$base/")" = 200 ] && [ "$(code "$base/style.css")" = 200 ] &&
		[ "$(code "$base/search-index.js")" = 200 ] || problem "the server: not the tree's pages"
	[ "$(code "$base/nosuch.html")" = 404 ] && [ "$(code --path-as-is "$base/../../etc/passwd")" = 404 ] || problem "the server: pages that are not there"
	curl -s "$base/ipl/WOpen.html" | grep -q 'ipl/gprocs/wopen' && curl -s "$base/ipl/index.html" | grep -q 'href="gprocs/wopen.html"' ||
		problem "the server: not the IPL's pages"
	[ "$(code -H 'Host: example.com' "$base/index.html")" = 403 ] || problem "the server: answered another Host (DNS rebinding)"
	token=$(curl -s "$base/index.html" | sed -n 's/.*data-run="\([0-9a-f]*\)".*/\1/p')
	printf 'procedure main()\n   write("ran ", 6 * 7)\n   exit(3)\nend\n' >"$tmp/prog.icn"
	[ ${#token} = 32 ] && [ "$(code -X POST --data-binary @"$tmp/prog.icn" "$base/run")" = 403 ] &&
		[ "$(code -X POST -H 'X-Gcon-Token: 0' --data-binary @"$tmp/prog.icn" "$base/run")" = 403 ] ||
		problem "the server: a run without its secret was not refused"
	"$G" doc -html run | grep -q 'data-run' && problem "gcon doc -html: a page with the server's secret"
	if [ -x "$here/libexec/gcon-vm" ]; then
		checks=$((checks + 2))
		stop
		GCONVM=$here/libexec/gcon-vm
		export GCONVM
		serve index
		token=$(curl -s "$base/index.html" | sed -n 's/.*data-run="\([0-9a-f]*\)".*/\1/p')
		curl -s -D "$tmp/hdr" -X POST -H "X-Gcon-Token: $token" --data-binary @"$tmp/prog.icn" "$base/run" >"$tmp/ran"
		[ "$(cat "$tmp/ran")" = "ran 42" ] && grep -q '^X-Gcon-Status: 3' "$tmp/hdr" ||
			problem "the server: a run gave $(cat "$tmp/ran"), $(grep Status "$tmp/hdr")"
		# Stop: the request abandoned, the program killed, its directory removed
		printf 'procedure main()\n   repeat write("x")\nend\n' >"$tmp/loop.icn"
		curl -s -o /dev/null --max-time 1 -X POST -H "X-Gcon-Token: $token" --data-binary @"$tmp/loop.icn" "$base/run"
		sleep 1
		[ -z "$(ls "$tmp/t")" ] || problem "the server: a stopped run left $(ls "$tmp/t")"
		unset GCONVM
	fi
	# the server stops when its last page has said bye (after a grace),
	# not before any page has said hello; with -http, never by itself
	checks=$((checks + 2))
	stop
	serve index
	token=$(curl -s "$base/index.html" | sed -n 's/.*data-run="\([0-9a-f]*\)".*/\1/p')
	curl -s -X POST --data "$token hello p1" "$base/alive" >/dev/null
	curl -s -X POST --data "$token bye p1" "$base/alive" >/dev/null
	n=0
	while kill -0 $server 2>/dev/null && [ $n -lt 100 ]; do
		sleep 0.1
		n=$((n + 1))
	done
	! kill -0 $server 2>/dev/null && [ $n -ge 30 ] && grep -q 'closed its pages' "$tmp/err" ||
		problem "the server: not stopped a grace after its last page went ($n tenths)"
	server=
	TMPDIR=$tmp/t "$G" doc -http 127.0.0.1:0 index >/dev/null 2>"$tmp/err" &
	server=$!
	sleep 1
	port=$(sed -n 's|.*http://127\.0\.0\.1:\([0-9]*\)/.*|\1|p' "$tmp/err")
	token=$(curl -s "http://127.0.0.1:$port/index.html" | sed -n 's/.*data-run="\([0-9a-f]*\)".*/\1/p')
	curl -s -X POST --data "$token hello p2" "http://127.0.0.1:$port/alive" >/dev/null
	curl -s -X POST --data "$token bye p2" "http://127.0.0.1:$port/alive" >/dev/null
	sleep 6
	kill -0 $server 2>/dev/null || problem "gcon doc -http: stopped by itself"
else
	echo "doc: no curl: the server's pages not checked"
fi
stop
checks=$((checks + 1))
BROWSER=false "$G" doc run >"$tmp/out" 2>"$tmp/err"
cmp -s "$tmp/out" "$tmp/text" && grep -q 'no browser' "$tmp/err" || problem "a failing browser: not the page as text, with a note"
echo "doc: $checks checks; $fail problems"
[ "$fail" = 0 ]
