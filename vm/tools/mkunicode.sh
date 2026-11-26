#!/bin/sh
# mkunicode.sh VERSION: regenerate src/unicode.c, gcon-vm's table of
# printable characters, from that version of the Unicode Character
# Database (UnicodeData.txt, fetched from unicode.org with curl into a
# temporary directory, and not kept). Run it when moving to a new
# Unicode version; never needed to build, and builds never use the
# network.
set -eu
[ $# -eq 1 ] || { echo "usage: tools/mkunicode.sh VERSION (e.g. 17.0.0)" >&2; exit 2; }
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --max-time 120 -o "$tmp/UnicodeData.txt" "https://www.unicode.org/Public/$1/ucd/UnicodeData.txt"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -o "$tmp/mkunicode" "$here/tools/mkunicode.c"
"$tmp/mkunicode" "$1" <"$tmp/UnicodeData.txt" >"$tmp/unicode.c"
mv "$tmp/unicode.c" "$here/src/unicode.c"
echo "wrote src/unicode.c (Unicode $1)"
