#!/bin/sh
# gcon's changes to the Icon Program Library in lib/ipl, applied by
# tools/ipl/update.sh after it copies a fresh IPL (and safe to run again:
# a file already changed is left alone). Each change replaces a line of
# upstream's that must occur exactly once; if upstream has changed so
# that it does not, the script stops and says so, for the change to be
# looked at again. The changed files are listed in lib/ipl/UPSTREAM.
#
#   tools/ipl/patch.sh
#
# The changes are icon-sdl's (github.com/phonologus/icon-sdl, branch
# sdl-graphics), whose SDL backend gcon's display server is: gcon
# predefines _SDL_GRAPHICS, not _X_WINDOW_SYSTEM, and its keys are X
# keysyms, so the key names are defined under _X_KEYSYMS, which either
# symbol defines:
#
#   gincl/keysyms.icn  defines _X_KEYSYMS and names the keys under it
#   gprocs/vtext.icn   tests _X_KEYSYMS for the keypad keys
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ipl=$(cd "$here/../../lib/ipl" && pwd)
changed=""

# change FILE OLD NEW REASON: replace the line OLD of FILE (exactly once)
# with the lines of file NEW.
change() {
	f=$ipl/$1
	if grep -q "^# gcon: " "$f"; then
		echo "patch.sh: $1: already changed"
		return
	fi
	n=$(grep -cxF -- "$2" "$f" || true)
	if [ "$n" != 1 ]; then
		echo "patch.sh: $1: expected the line '$2' once, found it $n times:" >&2
		echo "patch.sh: upstream has changed; look at this change again" >&2
		exit 1
	fi
	awk -v old="$2" -v new="$3" '
		$0 == old { while ((getline l < new) > 0) print l; next }
		{ print }' "$f" >"$f.new"
	mv "$f.new" "$f"
	changed="$changed $1"
	echo "patch.sh: $1: $4"
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

cat >"$tmp" <<'EOF'
# gcon: _X_KEYSYMS: keys are reported as X keysyms. That is so under X,
# and also with the SDL graphics backend of the icon-sdl fork and gcon,
# which report keys exactly as X does. (tools/ipl/patch.sh)
$ifdef _X_WINDOW_SYSTEM
$define _X_KEYSYMS
$endif
$ifdef _SDL_GRAPHICS
$define _X_KEYSYMS
$endif

$ifdef _X_KEYSYMS
EOF
change gincl/keysyms.icn '$ifdef _X_WINDOW_SYSTEM' "$tmp" \
	"key names under _X_KEYSYMS (X or SDL graphics)"

cat >"$tmp" <<'EOF'
# gcon: the keypad keys are X keysyms under _X_KEYSYMS (tools/ipl/patch.sh)
$ifndef _X_KEYSYMS
EOF
change gprocs/vtext.icn '$ifndef _X_WINDOW_SYSTEM' "$tmp" \
	"keypad keys under _X_KEYSYMS"

# Record the changed files in UPSTREAM (below the upstream hashes).
up=$ipl/UPSTREAM
for f in $changed; do
	grep -qx "patched $f" "$up" || echo "patched $f" >>"$up"
done
