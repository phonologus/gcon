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
#     the browser's pages have gone (not with -http); a terminal
#     browser in the foreground; a browser that fails, an error.
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

# The help pages' HTML links to the other help pages: the overview's
# commands and topics, and each "gcon help NAME" (not the title); the
# text is as gcon help gives it.
checks=$((checks + 3))
"$G" doc -html help/index >"$tmp/h"
grep -q '^	<a href="run.html">run</a>          run Icon source' "$tmp/h" && grep -q '^	<a href="unicode.html">unicode</a>  ' "$tmp/h" ||
	problem "gcon doc -html help/index: not linked to the commands and topics"
"$G" doc -html help/run >"$tmp/h"
grep -q 'See also: gcon help <a href="build.html">build</a>, gcon help <a href="library.html">library</a>' "$tmp/h" &&
	grep -q '<h1 id="[^"]*">gcon help run</h1>' "$tmp/h" || problem "gcon doc -html help/run: not linked to its see-alsos, or a linked title"
if "$G" doc -raw help/index | grep -q "<a "; then problem "gcon doc -raw help/index: links in the text"; fi
# every "gcon help NAME" and "gcon doc NAME" in the help pages names a
# page, and is linked to it (gcon doc ipl, served apart, is not)
for f in "$here"/src/help/*.txt; do
	n=$(basename "$f" .txt)
	[ "$n" = gcon ] && n=index
	"$G" doc -html "help/$n" | sed -n '/<pre>/,$p' >"$tmp/h"
	for ref in $(sed 's/[.,;:)"]/ /g' "$f" | grep -o 'gcon \(help\|doc\) [a-z][a-z0-9/-]*' | sed 's/ /_/g' | sort -u); do
		cmd=${ref%_*} name=${ref##*_}
		[ "$cmd $name" = "gcon_doc ipl" ] && continue
		[ "$cmd" = gcon_help ] && [ "$name" = "${n#index}" ] && continue
		checks=$((checks + 1))
		grep -q "$(echo "$cmd" | sed 's/_/ /') <a href=\"[^\"]*\">$name</a>" "$tmp/h" ||
			problem "gcon help $n: \"$(echo "$cmd" | sed 's/_/ /') $name\" not linked to a page"
	done
done

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
# browser that fails is an error. With curl: the pages, the
# IPL's, a run (with gcon-vm), and the guards on running.
root=$tmp/root
mkdir -p "$root/lib/ipl/procs" "$root/lib/ipl/gprocs" "$tmp/t"
cp "$here/lib/ipl/gprocs/wopen.icn" "$root/lib/ipl/gprocs/"
cp -R "$here/doc" "$root/doc"
rm -rf "$root/doc/html"
GCONROOT=$root
export GCONROOT
printf '#!/bin/sh\necho "$1" >"%s.new" && mv "%s.new" "%s"\n' "$tmp/url" "$tmp/url" "$tmp/url" >"$tmp/browser"
chmod +x "$tmp/browser"
# serve NAME: start gcon doc NAME, which gives the browser the URL and
# leaves the server in the background, its pid in the message
serve() {
	rm -f "$tmp/url"
	TMPDIR=$tmp/t BROWSER=$tmp/browser "$G" doc "$1" >"$tmp/out" 2>"$tmp/err" || problem "gcon doc $1: exit status $?: $(cat "$tmp/err")"
	server=$(sed -n 's/.*in the background (pid \([0-9]*\);.*/\1/p' "$tmp/err")
	url=$(cat "$tmp/url" 2>/dev/null)
	base=$(echo "$url" | sed -n 's|^\(http://127\.0\.0\.1:[0-9]*\)/.*|\1|p')
}
stop() {
	[ -n "$server" ] || return 0
	kill $server 2>/dev/null; wait $server 2>/dev/null # (-http's, a child)
	while kill -0 $server 2>/dev/null; do sleep 0.1; done
}
checks=$((checks + 3))
serve run
echo "$url" | grep -q '^http://127\.0\.0\.1:[0-9]*/help/run\.html$' || problem "BROWSER was not given help/run's URL: $url $(cat "$tmp/err")"
[ -n "$server" ] && kill -0 $server 2>/dev/null || problem "gcon doc: no server left in the background: $(cat "$tmp/err")"
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
	! kill -0 $server 2>/dev/null && [ $n -ge 30 ] ||
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
	# a terminal browser (lynx) runs in the foreground, its output kept,
	# and the server answers it while it runs; a failing browser is an
	# error, the page not shown, and its server gone
	checks=$((checks + 2))
	stop
	printf '#!/bin/sh\necho "$1" >"%s"\ncurl -s --max-time 5 -o /dev/null -w "%%{http_code}\\n" "$1"\n' "$tmp/url" >"$tmp/termbrowser"
	chmod +x "$tmp/termbrowser"
	BROWSER=$tmp/termbrowser "$G" doc run >"$tmp/out" 2>"$tmp/err"
	server=$(sed -n 's/.*in the background (pid \([0-9]*\);.*/\1/p' "$tmp/err")
	[ "$(cat "$tmp/out")" = 200 ] || problem "a terminal browser: got $(cat "$tmp/out") from the server, not 200: $(cat "$tmp/err")"
	stop
	printf '#!/bin/sh\necho "$1" >"%s"\nexit 3\n' "$tmp/url" >"$tmp/badbrowser"
	chmod +x "$tmp/badbrowser"
	BROWSER=$tmp/badbrowser "$G" doc run >"$tmp/out" 2>"$tmp/err"
	st=$?
	sleep 0.5
	[ $st = 1 ] && [ ! -s "$tmp/out" ] && grep -q 'exit status 3; use -text' "$tmp/err" && [ "$(code "$(cat "$tmp/url")")" = 000 ] ||
		problem "a failing browser: status $st, $(wc -c <"$tmp/out") bytes out, $(cat "$tmp/err"), the server still answering"
else
	echo "doc: no curl: the server's pages not checked"
fi
stop
# no browser at all (no display, no BROWSER: not on macOS, which has open)
if [ "$(uname)" != Darwin ]; then
	checks=$((checks + 1))
	env -u DISPLAY -u WAYLAND_DISPLAY -u BROWSER "$G" doc run >"$tmp/out" 2>"$tmp/err"
	st=$?
	[ $st = 1 ] && [ ! -s "$tmp/out" ] && grep -q 'no browser to open.*-text' "$tmp/err" ||
		problem "no browser: status $st, $(wc -c <"$tmp/out") bytes out, $(cat "$tmp/err")"
fi
echo "doc: $checks checks; $fail problems"
[ "$fail" = 0 ]
