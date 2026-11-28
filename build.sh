#!/bin/sh
# build.sh: build gcon into this checkout, which is a gcon root:
#
#   libexec/gcon-vm        the runtime (vm/; needs a C compiler)
#   libexec/gcon-graphics  the display server gcon-vm runs: the first named
#                          below, else the platform's own (none: no graphics)
#   libexec/gcon-graphics-NAME  each display server built (cocoa, x11, sdl)
#   bin/gcon               the command (src/)
#   bin/colrbook ... bin/vib  the programs Arizona builds into its bin
#                          (colrbook colrpick fontpick palette wevents
#                          xgamma vib), built by gcon from lib/ipl
#   lib/libcfunc.so        the IPL's C functions (lib/ipl/cfuncs)
#   lib/gcon/uclass.icn    gcon's uclass module, generated from deps/unicode
#                          (with doc/ref/uclass.md, its page, which is committed)
#   doc/html               the documentation as HTML (gcon doc -build)
#
#   ./build.sh [--withCocoa] [--withX11] [--withSDL]
#
# The display servers (graphics/design/design.md, "Native display
# servers"). With no flags, the platform's own: Cocoa on macOS, else X11
# where Xlib is found, else none, and gcon is built without graphics
# (programs that open a window stop with a message). The flags name the
# servers to build instead, the first being the one gcon-vm runs
# ($GCONGRAPHICS names another at run time):
#
#   --withCocoa  AppKit, Core Graphics and CoreText (graphics/cocoa/; macOS):
#                antialiased, at the display's scale
#   --withX11    Xlib (graphics/x11/): X's own drawing, Arizona Icon's pixels
#                (on macOS, under XQuartz)
#   --withSDL    SDL3 + Blend2D (graphics/; needs C++ and CMake): portable,
#                heavyweight; built only when named
#
# Builds never use the network. $CC and $CFLAGS are passed on.
set -eu
cd "$(dirname "$0")"
servers=
for a; do
	case $a in
	--withCocoa) servers="$servers cocoa" ;;
	--withX11) servers="$servers x11" ;;
	--withSDL) servers="$servers sdl" ;;
	*)
		echo "usage: ./build.sh [--withCocoa] [--withX11] [--withSDL]" >&2
		exit 2
		;;
	esac
done
if [ -z "$servers" ]; then
	if sh graphics/cocoa/build.sh -check; then
		servers=cocoa
	elif sh graphics/x11/build.sh -check; then
		servers=x11
	fi
fi
mkdir -p bin libexec

vm/build.sh
cp vm/bin/gcon-vm libexec/gcon-vm.new
mv libexec/gcon-vm.new libexec/gcon-vm
echo "built libexec/gcon-vm"

# lib/gcon/uclass.icn, the uclass module, from the vendored Unicode
# database (deps/unicode, checked against deps/deps.lock).
sh tools/uclass.sh

# put_server NAME BIN: the display server NAME, built as BIN; the first is
# libexec/gcon-graphics, the one gcon-vm runs
first=1
put_server() {
	cp "$2" "libexec/gcon-graphics-$1.new"
	mv "libexec/gcon-graphics-$1.new" "libexec/gcon-graphics-$1"
	echo "built libexec/gcon-graphics-$1"
	if [ $first = 1 ]; then
		cp "$2" libexec/gcon-graphics.new
		mv libexec/gcon-graphics.new libexec/gcon-graphics
		echo "libexec/gcon-graphics is gcon-graphics-$1"
		first=0
	fi
}
for s in $servers; do
	case $s in
	sdl)
		if command -v cmake >/dev/null 2>&1 && command -v "${CXX:-c++}" >/dev/null 2>&1; then
			mkdir -p graphics/build
			graphics/build.sh >graphics/build/build-all.log 2>&1 || {
				echo "build.sh: the SDL display server did not build: see graphics/build/build-all.log" >&2
				exit 1
			}
			put_server sdl graphics/bin/gcon-graphics
		else
			echo "build.sh: --withSDL: no C++ compiler or CMake" >&2
			exit 1
		fi
		;;
	x11)
		sh graphics/x11/build.sh >/dev/null || {
			echo "build.sh: --withX11: the X11 display server did not build (graphics/x11/build.sh)" >&2
			exit 1
		}
		put_server x11 graphics/bin/gcon-graphics-x11
		;;
	cocoa)
		sh graphics/cocoa/build.sh >/dev/null || {
			echo "build.sh: --withCocoa: the Cocoa display server did not build (graphics/cocoa/build.sh)" >&2
			exit 1
		}
		put_server cocoa graphics/bin/gcon-graphics-cocoa
		;;
	esac
done
if [ -z "$servers" ]; then
	rm -f libexec/gcon-graphics	# not an earlier build's
	echo "build.sh: no display server of this platform's (Cocoa is macOS's; X11 needs Xlib):"
	echo "build.sh: gcon is built without graphics (./build.sh --withSDL builds the portable server)"
fi

# The version, by Go's rules for its own: a commit tagged vX.Y.Z is
# vX.Y.Z; a later commit is the pseudo-version vX.Y.(Z+1)-0.DATE-COMMIT,
# which sorts after vX.Y.Z and before the next release (v0.0.0-DATE-COMMIT
# with no release tag behind it); "+dirty" marks uncommitted changes;
# "(devel)" without git.
version="(devel)"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
	release='v[0-9]*.[0-9]*.[0-9]*'
	if tag=$(git describe --tags --exact-match --match "$release" 2>/dev/null); then
		version=$tag
	else
		when=$(TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d%H%M%S)
		commit=$(git rev-parse HEAD | cut -c1-12)
		if tag=$(git describe --tags --abbrev=0 --match "$release" 2>/dev/null); then
			next=$(echo "$tag" | awk -F. '{ printf "%s.%s.%d", $1, $2, $3 + 1 }')
			version="$next-0.$when-$commit"
		else
			version="v0.0.0-$when-$commit"
		fi
	fi
	[ -n "$(git status --porcelain)" ] && version="$version+dirty"
fi
sh tools/embed.sh # src/*.inc: gcon help's pages, gcon doc's stylesheet and script
${CC:-cc} -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -DGCON_VERSION="\"$version\"" -Ivm/src \
	-o bin/gcon.new src/*.c vm/src/gen_tables.c
mv bin/gcon.new bin/gcon
echo "built bin/gcon ($version)"
# doc/html: the documentation, for gcon doc. A problem in the pages
# (gcon doc -check) leaves it unbuilt, and src/tests/doc.sh fails.
if bin/gcon doc -build; then
	echo "built doc/html"
else
	echo "build.sh: doc/html not built: see the problems above (gcon doc -check)" >&2
fi

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
