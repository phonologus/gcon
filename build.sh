#!/bin/sh
# build.sh: build gcon into this checkout, which is a gcon root:
#
#   libexec/gcon-vm        the runtime (vm/; needs a C compiler)
#   libexec/gcon-graphics  the display server (graphics/; needs C++ and
#                          CMake; skipped without them, and then programs
#                          cannot open windows)
#   bin/gcon               the command (src/)
#   bin/colrbook ... bin/vib  the programs Arizona builds into its bin
#                          (colrbook colrpick fontpick palette wevents
#                          xgamma vib), built by gcon from lib/ipl
#   lib/libcfunc.so        the IPL's C functions (lib/ipl/cfuncs)
#
# Builds never use the network. $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
mkdir -p bin libexec

vm/build.sh
cp vm/bin/gcon-vm libexec/gcon-vm.new
mv libexec/gcon-vm.new libexec/gcon-vm
echo "built libexec/gcon-vm"

if command -v cmake >/dev/null 2>&1 && command -v "${CXX:-c++}" >/dev/null 2>&1; then
	mkdir -p graphics/build
	graphics/build.sh >graphics/build/build-all.log 2>&1 || {
		echo "build.sh: the display server did not build: see graphics/build/build-all.log" >&2
		exit 1
	}
	cp graphics/bin/gcon-graphics libexec/gcon-graphics.new
	mv libexec/gcon-graphics.new libexec/gcon-graphics
	echo "built libexec/gcon-graphics"
else
	echo "build.sh: no C++ compiler or CMake: the display server is not built"
fi

# The version, in the form Go stamps its own: a pseudo-version naming
# the commit, "+dirty" for uncommitted changes; "(devel)" without git.
version="(devel)"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
	when=$(TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d%H%M%S)
	commit=$(git rev-parse HEAD | cut -c1-12)
	version="v0.0.0-$when-$commit"
	[ -n "$(git status --porcelain)" ] && version="$version+dirty"
fi
${CC:-cc} -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -DGCON_VERSION="\"$version\"" -Ivm/src \
	-o bin/gcon.new src/*.c vm/src/gen_tables.c
mv bin/gcon.new bin/gcon
echo "built bin/gcon ($version)"

# The C functions of the IPL (lib/ipl/cfuncs), as Arizona builds them
# into its bin, into lib/libcfunc.so, against gcon's icall.h; gcon-vm
# puts the root's lib on FPATH, where the IPL's cfunc.icn finds it.
cfuncs="bitcount external files fpoll internal lgconv osf pack ppm process tconnect"
ctmp=$(mktemp -d)
for f in $cfuncs; do
	${CC:-cc} -O2 -fPIC -w ${CFLAGS:-} -Ivm/include -c -o "$ctmp/$f.o" lib/ipl/cfuncs/$f.c
done
case "$(uname -s)" in
Darwin*) ${CC:-cc} -bundle -undefined dynamic_lookup -o lib/libcfunc.so.new "$ctmp"/*.o ;;
*) ${CC:-cc} -shared -fPIC -o lib/libcfunc.so.new "$ctmp"/*.o ;;
esac
rm -rf "$ctmp"
mv lib/libcfunc.so.new lib/libcfunc.so
echo "built lib/libcfunc.so"

# The programs Arizona's ipl/BuildBin builds into its bin: built by gcon
# from lib/ipl into the root's bin.
top=$(pwd)
for p in colrbook colrpick fontpick palette wevents xgamma; do
	GCONROOT=$top bin/gcon build -o bin/$p lib/ipl/gprogs/$p.icn
done
(cd lib/ipl/gpacks/vib && GCONROOT=$top "$top/bin/gcon" build -o "$top/bin/vib" vib.icn)
echo "built bin/colrbook bin/colrpick bin/fontpick bin/palette bin/wevents bin/xgamma bin/vib"
