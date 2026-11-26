#!/bin/sh
# Copy what gcon ships of the Icon Program Library from a local clone of
# the reference implementation into lib/ipl, recording the upstream
# commit and a SHA-256 manifest in lib/ipl/UPSTREAM:
#
#   procs gprocs incl gincl   the library: what link and $include find
#   gprogs/ gpacks/vib/       the programs Arizona's ipl/BuildBin builds
#                             into bin (colrbook colrpick fontpick
#                             palette wevents xgamma, and vib), which
#                             gcon's build.sh builds into the root's bin
#   cfuncs/                   the C functions (loadfunc), which build.sh
#                             builds into lib/libcfunc.so; their Icon
#                             interface, procs/cfunc.icn, is made here by
#                             cfuncs/mkfunc.sh, as Arizona's Makefile does
#
# The rest of the IPL (its other programs, packages, data and documents)
# is of historical and educational interest, and is in Arizona Icon's
# repository; gcon's tests read it there.
#
#   tools/ipl/update.sh [ICON_SRC]
#
# ICON_SRC defaults to ../icon relative to the repository root. It is
# read, never modified, and the network is never used. The IPL is public
# domain, except for the card bitmaps in gprocs/cardbits.icn, whose
# copyright and permission notices are reproduced in NOTICE, as they ask;
# a test checks that NOTICE carries them while the file is present.
# gcon's changes to it are then made by tools/ipl/patch.sh.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$(cd "${1:-$repo/../icon}" && pwd)
dst=$repo/lib/ipl
dirs="procs gprocs incl gincl"
# The programs of Arizona's ipl/BuildBin (GPROGS and GPACKS).
gprogs="colrbook colrpick fontpick palette wevents xgamma"
gpacks="vib"
# The C functions, as Arizona's cfuncs/Makefile builds them (but icall.h,
# which is gcon's own: vm/include/icall.h).
cfuncs="bitcount external files fpoll internal lgconv osf pack ppm process tconnect"

if command -v sha256sum >/dev/null 2>&1; then sha="sha256sum"; else sha="shasum -a 256"; fi
commit=$(git -C "$src" rev-parse HEAD)
date=$(git -C "$src" log -1 --format=%cs)
# (untracked ucode, which icont leaves where it runs, is no matter)
if [ -n "$(git -C "$src" status --porcelain -- ipl | grep -v '\.u[12]$')" ]; then
  echo "update.sh: $src/ipl has uncommitted changes" >&2
  exit 1
fi

rm -rf "${dst:?}/gprogs" "${dst:?}/gpacks" "${dst:?}/cfuncs"
for d in $dirs; do
  rm -rf "${dst:?}/$d"
  mkdir -p "$dst/$d"
  cp "$src/ipl/$d"/*.icn "$dst/$d/"
done
mkdir -p "$dst/gprogs"
for p in $gprogs; do cp "$src/ipl/gprogs/$p.icn" "$dst/gprogs/"; done
for p in $gpacks; do
  mkdir -p "$dst/gpacks/$p"
  cp "$src/ipl/gpacks/$p"/*.icn "$dst/gpacks/$p/"
done
mkdir -p "$dst/cfuncs"
for f in $cfuncs; do cp "$src/ipl/cfuncs/$f.c" "$dst/cfuncs/"; done
cp "$src/ipl/cfuncs/mkfunc.sh" "$src/ipl/cfuncs/README" "$dst/cfuncs/"
(cd "$dst/cfuncs" && LC_ALL=POSIX sh mkfunc.sh libcfunc.so $(for f in $cfuncs; do printf '%s.o ' "$f"; done)) \
  >"$dst/procs/cfunc.icn"

{
  echo "# The Icon Program Library's procedures and include files, the"
  echo "# programs Arizona builds into bin, and the C functions, copied"
  echo "# from github.com/gtownsend/icon by tools/ipl/update.sh, with gcon's"
  echo "# changes made by tools/ipl/patch.sh (the files listed as patched; the"
  echo "# hashes are upstream's). procs/cfunc.icn is made by cfuncs/mkfunc.sh."
  echo "# Do not edit: to change a file, copy it to a directory on IPATH (or"
  echo "# LPATH), or add a change to patch.sh."
  echo "commit $commit"
  echo "date $date"
  (cd "$dst" && for d in $dirs gprogs gpacks/* cfuncs; do $sha "$d"/*; done) | sed 's/^/sha256 /'
} > "$dst/UPSTREAM"
"$here/patch.sh"
echo "lib/ipl: $(grep -c '^sha256 ' "$dst/UPSTREAM") files from $commit"
