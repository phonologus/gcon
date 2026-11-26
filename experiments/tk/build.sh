#!/bin/sh
# build.sh: libicontk.so from icontk.c. Needs a C compiler, Tcl/Tk 9
# (found by pkg-config, or $TK_CFLAGS and $TK_LIBS) and Icon 9.5's
# icall.h (gcon-vm's include/, by default ../../vm/include, or
# $ICALL).
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
ICALL=${ICALL:-../../vm/include}
TK_CFLAGS=${TK_CFLAGS:-$(pkg-config --cflags tk)}
TK_LIBS=${TK_LIBS:-$(pkg-config --libs tk)}
case "$(uname -s)" in
Darwin*) SHARED="-bundle -undefined dynamic_lookup" ;;
*) SHARED="-shared -fPIC" ;;
esac
$CC -O2 -Wall -fPIC -I"$ICALL" $TK_CFLAGS $SHARED -o libicontk.so icontk.c $TK_LIBS
