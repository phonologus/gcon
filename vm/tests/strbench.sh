#!/bin/sh
# String benchmarks: Icon 9.5's string and cset operations on a subject
# of 1,000 bytes, built once by gcon and timed on gcon-vm, and built by
# icont and timed on iconx; ns per iteration (wall clock, the best of
# three), and gcon-vm's time as a fraction of iconx's: the bar the byte
# paths are measured against (docs/design.md, "Implementation: the byte
# path is iconx's"). As tests/bench.sh, which times single operations.
#
#   tests/strbench.sh [iterations]    # default 100000
#
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout), and
# iconx: $ICONBIN, or built by gcon's tools/iconx/build.sh; without it
# the iconx column is left out.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
fi
ICONBIN=${ICONBIN:-$(sh "${GCONSRC:-$here/..}/tools/iconx/build.sh" 2>/dev/null)}
[ -x "$ICONBIN/iconx" ] || ICONBIN=
N=${1:-100000}
${CC:-cc} -std=c11 -O1 -o "$tmp/besttime" "$here/tests/besttime.c" || exit 2
best() { "$tmp/besttime" "$@" || exit 2; }
printf '%-12s %9s %9s %8s\n' benchmark "iconx ns" "c ns" "c/iconx"
while IFS='|' read -r name body; do
	cat >"$tmp/b.icn" <<EOF
procedure main()
   local x, S, s, c, W
   S := repl("the quick brown fox jumps; ", 40)
   W := "the quick\tbrown\tfox"
   c := &letters
   every 1 to $N do $body
end
EOF
	(cd "$tmp" && "$GCON" build -o b b.icn) || exit 2
	c=$(best "$vm" "$tmp/b")
	i=0
	if [ "$ICONBIN" ]; then
		(cd "$tmp" && "$ICONBIN/icont" -s -o bi b.icn) || exit 2
		i=$(best "$ICONBIN/iconx" "$tmp/bi")
	fi
	awk -v n="$name" -v i="$i" -v c="$c" -v N="$N" 'BEGIN {
		printf "%-12s %9s %9.1f %8s\n", n, i ? sprintf("%.1f", i/N) : "-",
			c/N, i ? sprintf("%.2f", c/i) : "-" }'
done <<'EOF'
concat|x := S || "abc"
section|x := S[100:600]
streq|S == S[1:0]
bang|every !S
many|S ? while tab(upto(c)) do tab(many(c))
moveall|S ? while move(1)
reverse|x := reverse(S)
trim|x := trim(S || "      ")
repl|x := repl("ab", 500)
center|x := center("abc", 1000, "-")
map|x := map(S, &lcase, &ucase)
csetof|x := cset(S)
csetstr|x := string(&letters)
image|x := image(S)
detab|x := detab(W)
ordchar|x := char(ord("a"))
find|every find("fox", S)
size|x := *S
EOF
