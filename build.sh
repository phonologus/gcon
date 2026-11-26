#!/bin/sh
# build.sh: build gcon into this checkout, which is a gcon root:
#
#   libexec/gcon-vm        the runtime (vm/; needs a C compiler)
#   libexec/gcon-graphics  the display server (graphics/; needs C++ and
#                          CMake; skipped without them, and then programs
#                          cannot open windows)
#   bin/gcon               the command (for now the Go one, built if Go
#                          is present: it is being ported to C, see
#                          docs/design.md, "gcon in C")
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

if command -v go >/dev/null 2>&1; then
	go build -o bin/gcon ./cmd/gcon
	echo "built bin/gcon (Go)"
else
	echo "build.sh: no Go: bin/gcon is not built (until the C gcon replaces it)"
fi
