#!/bin/sh
# Micro-benchmarks: loops of single operations, each built once by gcon
# and timed on gcon-vm, and built by icont and timed on iconx; ns per
# iteration (wall clock, the best of three), and gcon-vm's time as a
# fraction of iconx's. Each includes starting the program, which the
# iteration count amortises.
#
#   tests/bench.sh [iterations]    # default 2000000
#
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout), and
# iconx: $ICONBIN (the directory of icont and iconx), or built by gcon's
# tools/iconx/build.sh; without it the iconx column is left out.
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
N=${1:-2000000}
${CC:-cc} -std=c11 -O1 -o "$tmp/besttime" "$here/tests/besttime.c" || exit 2
best() { "$tmp/besttime" "$@" || exit 2; } # best of three wall times, in ns
printf '%-12s %9s %9s %8s\n' benchmark "iconx ns" "c ns" "c/iconx"
while IFS='|' read -r name body; do
	cat >"$tmp/b.icn" <<EOF
procedure p()
end
procedure q(a, b)
   local c
   c := a
   return b
end
procedure main()
   local x, L, T, E, C
   E := []
   C := create |1
   x := 7
   L := [1, 2, 3]
   T := table()
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
empty|&null
size|*"abcde"
upto|"abcde" ? upto('d')
find|"abcde" ? find("de")
add|x + 407
call|p()
call2|q(1, 2)
subscript|L[3]
left|left("a", 10)
map|map("quick brown fox", &lcase, &ucase)
move|"abcde" ? move(1)
tableinsdel|insert(T, 5) & delete(T, 5)
push|pop(push(L, 0))
pushempty|pop(push(E, 0))
coswitch|@C
cocreate|@(create 1)
EOF
