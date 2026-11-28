#!/bin/sh
# The damage test: from each program given, gcon builds a file, and
# every variant of it below must give the same output, messages and exit
# status from gcon-vm -dis built here with the address and undefined
# behaviour sanitizers as from gcon dis (the root's gcon-vm, built
# without them):
#
#   - the encoded program truncated at each length;
#   - each byte of it changed (two ways), and the checksum then made
#     right again, so that the change reaches the decoder's other checks
#     and the validator rather than stopping at the checksum (and some
#     variants are valid programs, whose listings are compared).
#
# Needs gcon ($GCON, or built from $GCONSRC, default this checkout).
set -u
# gcon exits without freeing what it built, as icont does: no leak checks
# (ASan makes them on Linux, by default)
export ASAN_OPTIONS=detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd) || exit 2
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c" || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
${CC:-cc} -std=gnu11 -ffp-contract=off -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$here/../deps/stb" -o "$tmp/gcon-vm" "$here"/src/*.c -lm || exit 2
${CC:-cc} -std=c11 -O1 -o "$tmp/damage" "$here/tests/damage.c" || exit 2
pass=0 fail=0
for f in "$@"; do
	(cd "$(dirname "$f")" && "$GCON" build -o "$tmp/prog" "$(basename "$f")") || exit 2
	rm -rf "$tmp/v" && mkdir "$tmp/v"
	"$tmp/damage" "$tmp/prog" "$tmp/v" || exit 2
	for v in "$tmp"/v/*; do
		"$GCON" dis "$v" >"$tmp/want" 2>&1
		echo "status $?" >>"$tmp/want"
		"$tmp/gcon-vm" -dis "$v" >"$tmp/got" 2>&1
		echo "status $?" >>"$tmp/got"
		if cmp -s "$tmp/want" "$tmp/got"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
			if [ "$fail" -le 10 ]; then
				echo "FAIL $f variant $(basename "$v")"
				diff -a "$tmp/want" "$tmp/got" | head -6
			fi
		fi
	done
done
echo "damage: $pass identical, $fail differ"
[ "$fail" -eq 0 ]
