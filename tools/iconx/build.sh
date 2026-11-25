#!/bin/sh
# Build the reference Icon implementation (icont and iconx) in a private
# copy of the reference tree, for generating expected test output.
#
#   tools/iconx/build.sh [ICON_SRC] [WORKDIR]
#
# ICON_SRC defaults to ../icon (relative to the repo root); it is copied,
# never modified. WORKDIR (default: $TMPDIR/iconx-build) holds the copy.
# Prints the directory holding the built icont and iconx.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=${1:-$here/../../../icon}
work=${2:-${TMPDIR:-/tmp}/iconx-build}
case $(uname) in Darwin) cfg=macintosh ;; *) cfg=linux ;; esac

if [ ! -x "$work/bin/iconx" ]; then
  rm -rf "$work"
  mkdir -p "$work"
  (cd "$src" && tar --exclude=.git -cf - .) | (cd "$work" && tar xf -)
  (cd "$work" && make Configure name=$cfg >/dev/null 2>&1 && make >/dev/null 2>&1)
fi
echo "$work/bin"
