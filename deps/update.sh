#!/bin/sh
# Update gcon's vendored data: the only script that uses the network;
# build.sh never does, and verifies what this records in deps.lock.
#
#   deps/update.sh unicode [VERSION]
#
# fetches the Unicode Character Database files gcon uses, UnicodeData.txt
# and Scripts.txt, from unicode.org: of VERSION (as 17.0.0), or the
# latest, whose version is read from Scripts.txt's header so that the
# lock always names one. It replaces deps/unicode/, records the version
# and SHA-256s in deps/deps.lock, and regenerates gcon-vm's table of
# printable characters (vm/tools/mkunicode.sh) from the new files. The
# next build.sh regenerates lib/gcon/uclass.icn. Commit deps/ and
# vm/src/unicode.c together.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
die() { echo "update.sh: $*" >&2; exit 1; }
[ $# -ge 1 ] && [ "$1" = unicode ] && [ $# -le 2 ] || die "usage: deps/update.sh unicode [VERSION]"
ver=${2:-}

sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'; }

if [ -n "$ver" ]; then
	base="https://www.unicode.org/Public/$ver/ucd"
else
	base="https://www.unicode.org/Public/UCD/latest/ucd"
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in UnicodeData.txt Scripts.txt; do
	echo "fetching $base/$f"
	curl -fsSL --max-time 300 -o "$tmp/$f" "$base/$f" || die "download failed: $base/$f"
done
# Scripts.txt begins "# Scripts-17.0.0.txt"
got=$(sed -n '1s/^# Scripts-\([0-9][0-9.]*\)\.txt.*/\1/p' "$tmp/Scripts.txt")
[ -n "$got" ] || die "no version in Scripts.txt's header"
[ -z "$ver" ] || [ "$ver" = "$got" ] || die "asked for $ver, Scripts.txt says $got"
ver=$got
awk -F';' 'NF < 14 { bad = 1 } END { exit bad }' "$tmp/UnicodeData.txt" || die "UnicodeData.txt is malformed"

mkdir -p "$here/unicode"
mv "$tmp/UnicodeData.txt" "$tmp/Scripts.txt" "$here/unicode/"
{
	echo "# Vendored data: name, version, SHA-256 of deps/unicode/<file>, and"
	echo "# the URL it came from (@V@ = version). Change only with"
	echo "# deps/update.sh. build.sh verifies every checksum."
	for f in UnicodeData.txt Scripts.txt; do
		printf '%-16s %-8s %s https://www.unicode.org/Public/@V@/ucd/%s\n' \
			"$f" "$ver" "$(sha256 "$here/unicode/$f")" "$f"
	done
} >"$here/deps.lock"
sh "$here/../vm/tools/mkunicode.sh"
echo "Unicode $ver: deps/unicode, deps/deps.lock and vm/src/unicode.c updated"
