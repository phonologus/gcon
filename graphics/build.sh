#!/bin/sh
# Build gcon-graphics, the SDL server: src/main.c (the protocol reader)
# over src/plat.c's SDL3 windows and events, with gcon's own rasterizer
# and fonts (raster/: rcanvas.c, raster.c, rfont.c, outline fonts by
# stb_truetype from ../deps/stb, X's misc-fixed from ../deps/fixed, both
# checked against ../deps/deps.lock) and the platform's font lookup
# (src/fontmac.c, CoreText; src/fontlinux.c, fontconfig). Needs a C
# compiler and the system's SDL3 (and fontconfig, but on macOS), found
# by pkg-config. Never uses the network. The binary is left in
# bin/gcon-graphics. $CC and $CFLAGS are passed on.
#
#   build.sh          build
#   build.sh -check   only say whether SDL3 (and fontconfig) are found
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
deps=../deps
mac=
[ "$(uname)" = Darwin ] && mac=1
want="sdl3"
[ -z "$mac" ] && want="sdl3 fontconfig"
found() { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists $want 2>/dev/null; }
if [ "${1:-}" = -check ]; then
	found
	exit
fi
found || { echo "graphics/build.sh: no $want (pkg-config): install the development files" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'; }
grep -v '^#' "$deps/deps.lock" | awk 'index($1, "fixed/") == 1 || $1 == "stb/stb_truetype.h"' |
	while read -r name ver sha url; do
		[ "$(sha256 "$deps/$name")" = "$sha" ] ||
			{ echo "graphics/build.sh: deps/$name does not match deps/deps.lock" >&2; exit 1; }
	done
sh raster/bdf2c.sh "$deps"/fixed/*.bdf >"$tmp/fixedfonts.inc"
if [ -n "$mac" ]; then
	fontsrc=src/fontmac.c
	fontlibs="-framework CoreText -framework CoreFoundation -framework AppKit" # AppKit: plat.c's NSBeep
else
	fontsrc=src/fontlinux.c
	fontlibs=
fi
mkdir -p bin
# shellcheck disable=SC2086 # the flags are lists
$CC -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -DRFONT_LOOKUP -Isrc -Iraster -I"$deps/stb" -I"$tmp" \
	$(pkg-config --cflags $want) -o bin/gcon-graphics.new \
	src/main.c src/xcolors.c src/plat.c src/sdlgfx.c raster/raster.c raster/rcanvas.c raster/rfont.c \
	$fontsrc $(pkg-config --libs $want) $fontlibs -lpthread -lm
mv bin/gcon-graphics.new bin/gcon-graphics
echo "graphics/build.sh: built bin/gcon-graphics"
