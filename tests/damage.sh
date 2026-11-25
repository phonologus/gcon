#!/bin/sh
# The damage test: from each program given, gcon builds a file, and
# every variant of it below must give the same output, messages and exit
# status from gcon-vm -dis (built here with the address and undefined
# behaviour sanitizers) as from gcon dis:
#
#   - the encoded program truncated at each length;
#   - each byte of it changed (two ways), and the checksum then made
#     right again, so that the change reaches the decoder's other checks
#     and the validator rather than stopping at the checksum (and some
#     variants are valid programs, whose listings are compared).
#
# Needs gcon, as tests/dis.sh.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/../gcon}" && pwd) || exit 2
	(cd "$src" && go build -o "$tmp/gcon" ./cmd/gcon) || exit 2
	GCON=$tmp/gcon
	export GCONROOT="${GCONROOT:-$src}"
fi
${CC:-cc} -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$tmp/gcon-vm" "$here"/src/*.c -lm || exit 2
pass=0 fail=0
for f in "$@"; do
	(cd "$(dirname "$f")" && "$GCON" build -o "$tmp/prog" "$(basename "$f")") || exit 2
	rm -rf "$tmp/v" && mkdir "$tmp/v"
	perl -MCompress::Zlib -e '
		my ($in, $dir) = @ARGV;
		open my $fh, "<:raw", $in or die; local $/; my $d = <$fh>;
		my $start = index($d, "\n[gcon program follows]\n\x89GCN\r\n\x1a\n");
		die "not built" if $start < 0;
		$start += 1 + length("[gcon program follows]\n");
		my $n = 0;
		sub out { open my $o, ">:raw", sprintf("%s/%05d", $dir, $n++) or die; print $o $_[0]; }
		# Truncations, keeping the magic (else gcon reads source).
		out(substr($d, 0, $_)) for $start + 8 .. length($d) - 1;
		# Changed bytes, with the checksum made right when the "End "
		# section (its tag, length 4, the CRC) is untouched.
		my $end = length($d) - 9;
		for my $i ($start + 8 .. length($d) - 1) {
			for my $x (0x01, 0x80) {
				my $v = $d;
				substr($v, $i, 1) = chr(ord(substr($d, $i, 1)) ^ $x);
				if ($i < $end) {
					substr($v, -4) = pack("V", crc32(substr($v, $start, $end - $start)));
				}
				out($v);
			}
		}
	' "$tmp/prog" "$tmp/v" || exit 2
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
