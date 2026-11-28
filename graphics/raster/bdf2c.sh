#!/bin/sh
# bdf2c.sh: C tables (bdf.h's BdfFont) of the Latin-1 glyphs of BDF
# fonts, on standard output: bdffonts[], nbdffonts. Each file's name,
# without .bdf, names its font (7x13B: B bold, O oblique).
#
#   sh bdf2c.sh file.bdf ...
set -eu
[ $# -gt 0 ] || { echo "usage: bdf2c.sh file.bdf ..." >&2; exit 2; }
echo "/* Written by graphics/raster/bdf2c.sh from $(for f; do basename "$f"; done | tr '\n' ' ')*/"
echo '#include "bdf.h"'
n=0
names=
for f; do
	name=$(basename "$f" .bdf)
	id=$(echo "$name" | tr -c 'A-Za-z0-9\n' '_')
	awk -v name="$name" -v id="$id" '
		function hexbyte(s) { return "0x" s }
		$1 == "PIXEL_SIZE" { pixel = $2 }
		$1 == "FONT_ASCENT" { ascent = $2 }
		$1 == "FONT_DESCENT" { descent = $2 }
		$1 == "ENCODING" { enc = $2 + 0 }
		$1 == "DWIDTH" { dw = $2 }
		$1 == "BBX" { w = $2; h = $3; xo = $4; yo = $5 }
		$1 == "BITMAP" { inbits = 1; start[enc] = nbytes; next }
		$1 == "ENDCHAR" {
			inbits = 0
			if (enc >= 0 && enc <= 255) {
				g[enc] = sprintf("{%d, %d, %d, %d, %d, %d}", dw, w, h, xo, yo, start[enc])
				if (dw > maxw) maxw = dw
			}
			next
		}
		inbits {
			for (i = 1; i < length($1); i += 2) {
				bytes = bytes (nbytes % 16 ? " " : "\n\t") hexbyte(substr($1, i, 2)) ","
				nbytes++
			}
		}
		END {
			printf "static const unsigned char bits_%s[] = {%s\n\t0\n};\n", id, bytes
			printf "static const BdfGlyph glyphs_%s[256] = {\n", id
			for (c = 0; c < 256; c++)
				printf "\t%s,\n", (c in g) ? g[c] : "{0, 0, 0, 0, 0, -1}"
			printf "};\n"
			b = name ~ /B$/; o = name ~ /O$/
			printf "#define FONT_%s {\"%s\", %d, %d, %d, %d, %d, %d, glyphs_%s, bits_%s}\n", \
				id, name, pixel, ascent, descent, maxw, b, o, id, id
		}
	' "$f"
	names="$names FONT_$id,"
	n=$((n + 1))
done
echo "static const BdfFont bdffonts[] = {$names };"
echo "enum { nbdffonts = $n };"
