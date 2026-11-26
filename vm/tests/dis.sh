#!/bin/sh
# The loader test: gcon builds each program given (directories are
# searched for *.icn), and gcon-vm's listing of the built file must be
# identical to gcon's (`gcon dis`). Programs gcon cannot build are
# skipped. Needs gcon: $GCON, or built from $GCONSRC (default .., this checkout),
# whose tree is then its root (the library).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
pass=0 fail=0 skip=0
for arg in "$@"; do
	if [ -d "$arg" ]; then find "$arg" -name '*.icn'; else echo "$arg"; fi
done | sort >"$tmp/list"
while read -r f; do
	if ! (cd "$(dirname "$f")" && "$GCON" build -o "$tmp/prog" "$(basename "$f")") >/dev/null 2>&1; then
		skip=$((skip + 1))
		continue
	fi
	"$GCON" dis "$tmp/prog" >"$tmp/want" 2>&1
	"$vm" -dis "$tmp/prog" >"$tmp/got" 2>&1
	if cmp -s "$tmp/want" "$tmp/got"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $f"
		diff -a "$tmp/want" "$tmp/got" | head -10
	fi
done <"$tmp/list"
echo "dis: $pass identical, $fail differ, $skip not built"
[ "$fail" -eq 0 ]
