#!/bin/sh
# build.sh: build gcon into this checkout, which is a gcon root:
#
#   libexec/gcon-vm        the runtime (vm/; needs a C compiler)
#   libexec/gcon-graphics  the display server (graphics/; needs C++ and
#                          CMake; skipped without them, and then programs
#                          cannot open windows)
#   bin/gcon-c             the command, in C (src/): it becomes bin/gcon
#                          when the Go reference goes (docs/design.md,
#                          "gcon in C")
#   bin/gcon               the command, the Go reference, built if Go is
#                          present
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

# The version, as Go stamps its own: a pseudo-version naming the commit,
# "+dirty" for uncommitted changes; "(devel)" without git.
version="(devel)"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
	when=$(TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d%H%M%S)
	commit=$(git rev-parse HEAD | cut -c1-12)
	version="v0.0.0-$when-$commit"
	[ -n "$(git status --porcelain)" ] && version="$version+dirty"
fi
${CC:-cc} -std=c11 -O2 -Wall -Wextra ${CFLAGS:-} -DGCON_VERSION="\"$version\"" -Ivm/src \
	-o bin/gcon-c.new src/*.c vm/src/gen_tables.c
mv bin/gcon-c.new bin/gcon-c
echo "built bin/gcon-c ($version)"

if command -v go >/dev/null 2>&1; then
	go build -o bin/gcon ./cmd/gcon
	echo "built bin/gcon (Go)"
else
	echo "build.sh: no Go: bin/gcon is not built (until the C gcon replaces it)"
fi
