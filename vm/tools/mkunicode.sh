#!/bin/sh
# mkunicode.sh: regenerate src/unicode.c, gcon-vm's table of printable
# characters, from the Unicode Character Database vendored in gcon's
# deps/unicode (UnicodeData.txt, at the version deps/deps.lock records).
# deps/update.sh unicode runs it when moving to a new Unicode version;
# never needed to build, and never uses the network.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
deps=$here/../deps
ver=$(awk '$1 == "UnicodeData.txt" { print $2 }' "$deps/deps.lock")
[ -n "$ver" ] || { echo "mkunicode.sh: no UnicodeData.txt in deps/deps.lock" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -O2 -Wall -Wextra -o "$tmp/mkunicode" "$here/tools/mkunicode.c"
"$tmp/mkunicode" "$ver" <"$deps/unicode/UnicodeData.txt" >"$tmp/unicode.c"
mv "$tmp/unicode.c" "$here/src/unicode.c"
echo "wrote src/unicode.c (Unicode $ver)"
