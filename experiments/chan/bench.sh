#!/bin/sh
# bench.sh: ring and sieve on gcon-vm, gcon's Go VM and iconx, with peak
# memory. Needs gcon (../../bin/gcon), gcon-vm ($GCONVM, default
# ../../vm/bin/gcon-vm) and iconx ($ICONBIN, else built by
# gcon's tools/iconx/build.sh).
set -eu
cd "$(dirname "$0")"
gcon=../../bin/gcon
vm=${GCONVM:-../../vm/bin/gcon-vm}
ICONBIN=${ICONBIN:-$(sh ../../tools/iconx/build.sh)}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for p in ring sieve; do
	$gcon build -o "$tmp/$p" $p.icn
	(cp chan.icn $p.icn "$tmp" && cd "$tmp" && "$ICONBIN/icont" -s -c chan.icn && "$ICONBIN/icont" -s -o $p-x $p.icn)
done
run() { # label command...: the program's timing line and peak memory
	label=$1; shift
	if ! /usr/bin/time -l "$@" 2>"$tmp/err" >/dev/null; then
		printf '%-8s failed: %s\n' "$label" "$(grep -v '^ ' "$tmp/err" | grep -v 'real' | head -3 | tr '\n' ' ')"
		return
	fi
	printf '%-8s %s, %s MB\n' "$label" "$(grep ' ms' "$tmp/err")" \
		"$(awk '/maximum resident/ {printf "%.0f", $1 / 1048576}' "$tmp/err")"
}
while read -r prog args; do
	echo "== $prog $args"
	run gcon-vm "$vm" "$tmp/$prog" $args
	# iconx's collector crashes (bus error) with many co-expressions on
	# this platform: regions this large keep it from collecting.
	run iconx env BLKSIZE=400000000 STRSIZE=50000000 "$ICONBIN/iconx" "$tmp/$prog-x" $args
done <<'END'
ring 1000 1000
ring 10 100000
ring 100000 10
sieve 1000
sieve 3000
END
