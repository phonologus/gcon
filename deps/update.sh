#!/bin/sh
# Update gcon's vendored dependencies: the only script that uses the
# network; builds never do, and verify what this records in deps.lock
# (each line: a path under deps/, its version, SHA-256 and source URL,
# @V@ standing for the version).
#
#   deps/update.sh unicode [VERSION]
#
# fetches the Unicode Character Database files gcon uses, UnicodeData.txt
# and Scripts.txt, from unicode.org, with Unicode's licence (license.txt,
# kept as LICENSE: the Unicode License v3 asks that its notice go with
# copies of the data): of VERSION (as 17.0.0), or the
# latest, whose version is read from Scripts.txt's header so that the
# lock always names one. It replaces deps/unicode/, records the version
# and SHA-256s in deps/deps.lock, and regenerates gcon-vm's table of
# printable characters (vm/tools/mkunicode.sh) from the new files. The
# next build.sh regenerates lib/gcon/uclass.icn. Commit deps/ and
# vm/src/unicode.c together.
#
#   deps/update.sh stb [COMMIT]
#
# fetches stb_image.h and stb_image_write.h, Sean Barrett's image
# decoders and encoders, which gcon-vm's image files use
# (vm/src/imgcodec.c), with the repository's LICENSE, from
# github.com/nothings/stb at COMMIT (a full SHA), or at the head of its
# master branch (stb makes no releases): into deps/stb/, the commit
# recorded in deps/deps.lock. stb is in the public domain (or MIT, as
# its user chooses; gcon takes it as public domain).
#
#   deps/update.sh fixed [COMMIT]
#
# fetches X's misc-fixed bitmap fonts (the misc-misc fonts, X's own
# "fixed" among them), which the headless display server draws text
# with, from gitlab.freedesktop.org/xorg/font/misc-misc at COMMIT (a full
# SHA), or at the head of its default branch, with their COPYING (they
# are in the public domain). The originals hold some 4000 Unicode glyphs
# each; gcon keeps their Latin-1 ones (encodings 0 to 255, as Icon's
# text is drawn), still as BDF, in deps/fixed/, the commit recorded in
# deps/deps.lock with the SHA-256 of each file kept.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
die() { echo "update.sh: $*" >&2; exit 1; }
usage="usage: deps/update.sh unicode [VERSION] | stb [COMMIT] | fixed [COMMIT]"
[ $# -ge 1 ] && [ $# -le 2 ] || die "$usage"
what=$1
ver=${2:-}

sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() {
	echo "fetching $1"
	curl -fsSL --max-time 300 -o "$2" "$1" || die "download failed: $1"
}

# relock DIR LINES: deps.lock with DIR's lines replaced by those in file LINES
relock() {
	{
		echo "# Vendored dependencies: a path under deps/, its version, SHA-256,"
		echo "# and the URL it came from (@V@ = version). Change only with"
		echo "# deps/update.sh. Builds verify every checksum."
		grep -v '^#' "$here/deps.lock" 2>/dev/null | awk -v d="$1/" 'index($1, d) != 1'
		cat "$2"
	} >"$tmp/lock"
	mv "$tmp/lock" "$here/deps.lock"
}

case $what in
unicode)
	if [ -n "$ver" ]; then
		base="https://www.unicode.org/Public/$ver/ucd"
	else
		base="https://www.unicode.org/Public/UCD/latest/ucd"
	fi
	for f in UnicodeData.txt Scripts.txt; do
		fetch "$base/$f" "$tmp/$f"
	done
	# Scripts.txt begins "# Scripts-17.0.0.txt"
	got=$(sed -n '1s/^# Scripts-\([0-9][0-9.]*\)\.txt.*/\1/p' "$tmp/Scripts.txt")
	[ -n "$got" ] || die "no version in Scripts.txt's header"
	[ -z "$ver" ] || [ "$ver" = "$got" ] || die "asked for $ver, Scripts.txt says $got"
	ver=$got
	awk -F';' 'NF < 14 { bad = 1 } END { exit bad }' "$tmp/UnicodeData.txt" || die "UnicodeData.txt is malformed"
	fetch https://www.unicode.org/license.txt "$tmp/LICENSE"
	grep -q "UNICODE LICENSE V3" "$tmp/LICENSE" || die "license.txt is not the Unicode License v3: read it before going on"
	mkdir -p "$here/unicode"
	mv "$tmp/UnicodeData.txt" "$tmp/Scripts.txt" "$tmp/LICENSE" "$here/unicode/"
	for f in UnicodeData.txt Scripts.txt; do
		printf '%-24s %-8s %s https://www.unicode.org/Public/@V@/ucd/%s\n' \
			"unicode/$f" "$ver" "$(sha256 "$here/unicode/$f")" "$f"
	done >"$tmp/lines"
	printf '%-24s %-8s %s https://www.unicode.org/license.txt\n' unicode/LICENSE "$ver" \
		"$(sha256 "$here/unicode/LICENSE")" >>"$tmp/lines"
	relock unicode "$tmp/lines"
	sh "$here/../vm/tools/mkunicode.sh"
	echo "Unicode $ver: deps/unicode, deps/deps.lock and vm/src/unicode.c updated"
	;;
stb)
	if [ -z "$ver" ]; then
		ver=$(curl -fsSL --max-time 60 https://api.github.com/repos/nothings/stb/commits/master |
			sed -n 's/^  "sha": "\([0-9a-f]*\)",$/\1/p' | head -1)
		[ -n "$ver" ] || die "no head commit for github.com/nothings/stb"
	fi
	echo "$ver" | grep -q '^[0-9a-f]\{40\}$' || die "COMMIT must be a full SHA: $ver"
	base="https://raw.githubusercontent.com/nothings/stb/$ver"
	for f in stb_image.h stb_image_write.h LICENSE; do
		fetch "$base/$f" "$tmp/$f"
	done
	grep -q "public domain" "$tmp/LICENSE" || die "stb's LICENSE no longer offers the public domain: read it before going on"
	mkdir -p "$here/stb"
	mv "$tmp/stb_image.h" "$tmp/stb_image_write.h" "$tmp/LICENSE" "$here/stb/"
	for f in stb_image.h stb_image_write.h LICENSE; do
		printf '%-24s %-8s %s https://raw.githubusercontent.com/nothings/stb/@V@/%s\n' \
			"stb/$f" "$ver" "$(sha256 "$here/stb/$f")" "$f"
	done >"$tmp/lines"
	relock stb "$tmp/lines"
	echo "stb $ver: deps/stb and deps/deps.lock updated"
	;;
fixed)
	fonts="5x8 6x10 6x13 6x13B 6x13O 7x13 7x13B 7x13O 7x14 7x14B 8x13 8x13B 8x13O 9x15 9x15B 9x18 9x18B 10x20"
	api=https://gitlab.freedesktop.org/api/v4/projects/xorg%2Ffont%2Fmisc-misc/repository/commits
	if [ -z "$ver" ]; then
		ver=$(curl -fsSL --max-time 60 "$api?per_page=1" | sed -n 's/^\[{"id":"\([0-9a-f]*\)".*/\1/p')
		[ -n "$ver" ] || die "no head commit for xorg/font/misc-misc"
	fi
	echo "$ver" | grep -q '^[0-9a-f]\{40\}$' || die "COMMIT must be a full SHA: $ver"
	base="https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/$ver"
	fetch "$base/COPYING" "$tmp/COPYING"
	grep -q "Public domain font" "$tmp/COPYING" || die "misc-misc's COPYING no longer says public domain: read it before going on"
	mkdir -p "$tmp/fixed"
	for f in $fonts; do
		fetch "$base/$f.bdf" "$tmp/$f.bdf"
		# the header (CHARS counting the glyphs kept), the glyphs
		# encoded 0 to 255, ENDFONT
		awk -v src="$f.bdf" -v ver="$ver" '
			/^STARTFONT/ { print; print "COMMENT the Latin-1 glyphs (0 to 255) of xorg/font/misc-misc " src " at " ver ", kept by gcon'"'"'s deps/update.sh"; next }
			/^CHARS / { inhead = 0; next }
			/^STARTCHAR/ { buf = $0 "\n"; inchar = 1; next }
			inchar { buf = buf $0 "\n"; if ($1 == "ENCODING") enc = $2 + 0
				if ($1 == "ENDCHAR") { inchar = 0; if (enc >= 0 && enc <= 255) { glyphs = glyphs buf; n++ } }
				next }
			/^ENDFONT/ { next }
			{ head = head $0 "\n" }
			END { printf "%s", head; print "CHARS " n; printf "%s", glyphs; print "ENDFONT" }
		' "$tmp/$f.bdf" >"$tmp/fixed/$f.bdf"
		grep -q '^ENCODING 65$' "$tmp/fixed/$f.bdf" || die "$f.bdf: no glyph for A"
	done
	cp "$tmp/COPYING" "$tmp/fixed/COPYING"
	rm -rf "$here/fixed"
	mv "$tmp/fixed" "$here/fixed"
	for f in $fonts; do
		printf '%-24s %-8s %s https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/@V@/%s (Latin-1)\n' \
			"fixed/$f.bdf" "$ver" "$(sha256 "$here/fixed/$f.bdf")" "$f.bdf"
	done >"$tmp/lines"
	printf '%-24s %-8s %s https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/@V@/COPYING\n' \
		fixed/COPYING "$ver" "$(sha256 "$here/fixed/COPYING")" >>"$tmp/lines"
	relock fixed "$tmp/lines"
	echo "misc-fixed $ver: deps/fixed and deps/deps.lock updated"
	;;
*)
	die "$usage"
	;;
esac
