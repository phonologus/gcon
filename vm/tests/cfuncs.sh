#!/bin/sh
# Builds the IPL's C functions (ipl/cfuncs, from $ICONSRC, by default
# ../icon beside this repository) against include/icall.h, as their
# Makefile does, into a library and cfunc.icn, then runs tests/cfuncs.icn
# on gcon-vm and compares its output with tests/cfuncs.std. With iconx
# ($ICONBIN, else gcon's tools/iconx/build.sh) it also runs the same
# program on iconx with the same library, and shows any difference.
#
# Needs a C compiler, gcon ($GCON, or built from $GCONSRC, default this
# checkout) and the Icon sources.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
vm=${VM:-$here/bin/gcon-vm}
icon=$(cd "${ICONSRC:-$here/../../icon}" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "${GCON:-}" ]; then
	src=$(cd "${GCONSRC:-$here/..}" && pwd)
	${CC:-cc} -std=c11 -O2 -DGCON_VERSION='"test"' -I"$src/vm/src" -o "$tmp/gcon" "$src"/src/*.c "$src/vm/src/gen_tables.c"
	GCON=$tmp/gcon
	export GCONROOT="$src" # for the IPL (link io)
fi
CC=${CC:-cc}
funcs="bitcount external files fpoll internal lgconv osf pack ppm process tconnect"
cp "$icon"/ipl/cfuncs/*.c "$icon"/ipl/cfuncs/mkfunc.sh "$here/include/icall.h" \
	"$here/tests/cfuncs.icn" "$tmp"
cd "$tmp"
for f in $funcs; do
	$CC -O2 -fPIC -w -c $f.c
done
case "$(uname -s)" in
Darwin*) $CC -bundle -undefined dynamic_lookup -o libcfunc.so *.o ;;
*) $CC -shared -fPIC -o libcfunc.so *.o ;;
esac
sh mkfunc.sh libcfunc.so $(for f in $funcs; do printf '%s.o ' $f; done) >cfunc.icn
IPATH=$tmp "$GCON" build -o cfuncs cfuncs.icn
FPATH=$tmp "$vm" cfuncs >got.out 2>/dev/null
if diff "$here/tests/cfuncs.std" got.out; then
	echo "cfuncs: pass"
else
	echo "cfuncs: FAIL"
	exit 1
fi
ICONBIN=${ICONBIN:-$(sh "${GCONSRC:-$here/..}/tools/iconx/build.sh" 2>/dev/null || true)}
if [ -x "${ICONBIN:-}/iconx" ]; then
	cp "$icon/ipl/procs/io.icn" .
	"$ICONBIN/icont" -s -c io.icn cfunc.icn
	"$ICONBIN/icont" -s -o cfuncs-x cfuncs.icn
	FPATH=$tmp "$ICONBIN/iconx" ./cfuncs-x >iconx.out 2>/dev/null || true
	# Expected: iconx's default image of an external without data is
	# garbled (docs/design.md, "loadfunc").
	if diff iconx.out got.out >diff.out; then
		echo "cfuncs: identical to iconx"
	else
		echo "cfuncs: differences from iconx:"
		cat diff.out
	fi
fi
