#!/bin/sh
# Regenerate vm/src/gen_tables.c from gcon's Go sources (the names of
# gcon's builtins, Go's Unicode printability). Run it when gcon gains a
# builtin or changes Go version (and so Unicode version). Never needed
# to build. (Until the C front end replaces the Go: then these tables
# come from the C.)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
gcon=$(cd "$here/.." && pwd)
rev=$(git -C "$gcon" rev-parse --short HEAD)
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
(cd "$gcon" && go run ./vm/tools/gentables "$rev") >"$tmp"
mv "$tmp" "$here/src/gen_tables.c"
