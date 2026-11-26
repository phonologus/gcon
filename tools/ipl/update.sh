#!/bin/sh
# Copy the Icon Program Library's procedures and include files from a
# local clone of the reference implementation into lib/ipl, recording the
# upstream commit and a SHA-256 manifest in lib/ipl/UPSTREAM.
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
# Files not to copy, as "dir/file.icn ...".
exclude=""

if command -v sha256sum >/dev/null 2>&1; then sha="sha256sum"; else sha="shasum -a 256"; fi
commit=$(git -C "$src" rev-parse HEAD)
date=$(git -C "$src" log -1 --format=%cs)
if [ -n "$(git -C "$src" status --porcelain -- ipl)" ]; then
  echo "update.sh: $src/ipl has uncommitted changes" >&2
  exit 1
fi

for d in $dirs; do
  rm -rf "${dst:?}/$d"
  mkdir -p "$dst/$d"
  cp "$src/ipl/$d"/*.icn "$dst/$d/"
done
for f in $exclude; do rm -f "$dst/$f"; done

{
  echo "# The Icon Program Library's procedures and include files, copied"
  echo "# from github.com/gtownsend/icon by tools/ipl/update.sh, with gcon's"
  echo "# changes made by tools/ipl/patch.sh (the files listed as patched; the"
  echo "# hashes are upstream's). Do not edit: to change a file, copy it to a"
  echo "# directory on IPATH (or LPATH), or add a change to patch.sh."
  echo "commit $commit"
  echo "date $date"
  for f in $exclude; do echo "excluded $f"; done
  (cd "$dst" && for d in $dirs; do $sha "$d"/*.icn; done) | sed 's/^/sha256 /'
} > "$dst/UPSTREAM"
"$here/patch.sh"
echo "lib/ipl: $(grep -c '^sha256 ' "$dst/UPSTREAM") files from $commit"
