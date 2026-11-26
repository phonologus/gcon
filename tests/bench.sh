#!/bin/sh
# The micro-benchmarks of gcon's internal/run/vmbench_test.go: loops of
# single operations, each built once by gcon and timed on gcon's Go VM
# and on gcon-vm, and built by icont and timed on iconx; ns per
# iteration (wall clock, the best of three), and gcon-vm's time as a
# fraction of the Go VM's and of iconx's. Each includes starting the
# program, which the iteration count amortises.
#
#   tests/bench.sh [iterations]    # default 2000000
#
# Needs gcon, as tests/dis.sh, and iconx: $ICONBIN (the directory of
# icont and iconx), or built by gcon's tools/iconx/build.sh; without it
# the iconx column is left out.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/../gcon}" && pwd) || exit 2
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
fi
ICONBIN=${ICONBIN:-$(sh "${GCONSRC:-$here/../gcon}/tools/iconx/build.sh" 2>/dev/null)}
[ -x "$ICONBIN/iconx" ] || ICONBIN=
N=${1:-2000000}
best() { # best of three wall times, in ns, of "$@"
	perl -MTime::HiRes=time -e '
		my $best;
		for (1..3) { my $t = time; system(@ARGV) == 0 or die "failed: @ARGV\n"; $t = time - $t;
			$best = $t if !defined $best || $t < $best }
		printf "%.0f\n", $best * 1e9' "$@"
}
printf '%-12s %9s %9s %9s %8s %8s\n' benchmark "iconx ns" "go ns" "c ns" "c/go" "c/iconx"
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
	g=$(GCONVM=go best "$GCON" run "$tmp/b")
	c=$(best "$vm" "$tmp/b")
	i=0
	if [ "$ICONBIN" ]; then
		(cd "$tmp" && "$ICONBIN/icont" -s -o bi b.icn) || exit 2
		i=$(best "$ICONBIN/iconx" "$tmp/bi")
	fi
	perl -e 'my ($n, $i, $g, $c, $N) = @ARGV;
		printf "%-12s %9s %9.1f %9.1f %8.2f %8s\n", $n, $i ? sprintf("%.1f", $i/$N) : "-",
			$g/$N, $c/$N, $c/$g, $i ? sprintf("%.2f", $c/$i) : "-"' \
		"$name" "$i" "$g" "$c" "$N"
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
