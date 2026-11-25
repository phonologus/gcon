#!/bin/sh
# Copy the Icon Program Library's procedures and include files from a
# local clone of the reference implementation into lib/ipl, recording the
# upstream commit and a SHA-256 manifest in lib/ipl/UPSTREAM.
#
#   tools/ipl/update.sh [ICON_SRC]
#
# ICON_SRC defaults to ../icon relative to the repository root. It is
# read, never modified, and the network is never used. Only public-domain
# files are copied: those listed in $exclude carry other licences.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=$(cd "${1:-$repo/../icon}" && pwd)
dst=$repo/lib/ipl
dirs="procs gprocs incl gincl"
# gprocs/cardbits.icn: card bitmaps under an X11-style licence (Sun
# Microsystems and others), not public domain.
exclude="gprocs/cardbits.icn"

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
  echo "# from github.com/gtownsend/icon by tools/ipl/update.sh. Do not edit:"
  echo "# to change a file, copy it to a directory on IPATH (or LPATH)."
  echo "commit $commit"
  echo "date $date"
  for f in $exclude; do echo "excluded $f"; done
  (cd "$dst" && for d in $dirs; do $sha "$d"/*.icn; done) | sed 's/^/sha256 /'
} > "$dst/UPSTREAM"
echo "lib/ipl: $(grep -c '^sha256 ' "$dst/UPSTREAM") files from $commit"
