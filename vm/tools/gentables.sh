#!/bin/sh
# Regenerate src/gen_tables.c from gcon's sources ($GCONSRC, default
# ../gcon beside this repository). Run it when gcon gains a builtin or
# changes Go version (and so Unicode version). Never needed to build.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
gcon=$(cd "${GCONSRC:-$here/../gcon}" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '{"Replace":{"%s/cmd/gcon-vm-gentables/main.go":"%s/tools/gentables/main.go"}}\n' \
	"$gcon" "$here" >"$tmp/overlay.json"
rev=$(git -C "$gcon" rev-parse --short HEAD)
(cd "$gcon" && go run -overlay "$tmp/overlay.json" ./cmd/gcon-vm-gentables "$rev") >"$tmp/out.c"
mv "$tmp/out.c" "$here/src/gen_tables.c"
