#!/bin/sh
# iplclosure.sh: the files of the Icon Program Library (lib/ipl) that
# programs need: the files they link and $include, and the files those
# link and include, to the end. With no arguments, the programs Gcon
# builds and ships (lib/ipl/gprogs, vib) and link cfunc: the IPL that
# Gcon's own distribution depends on, from which doc/ipl-candidates.txt
# (candidates for Gcon's standard library) was first made.
# design/design.md, "What of the IPL is essential".
#
#   tools/iplclosure.sh [file.icn ...]    # files relative to lib/ipl
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/lib/ipl"
[ $# -gt 0 ] || set -- gprogs/*.icn gpacks/vib/vib.icn procs/cfunc.icn
links() { sed -n 's/^[ \t]*link[ \t]\{1,\}\(.*\)/\1/p' "$1" | sed 's/#.*//' | tr ',' '\n' | tr -d ' \t"'; }
incs() { sed -n 's/^[ \t]*\$include[ \t]\{1,\}"\{0,1\}\([^" \t]*\).*/\1/p' "$1"; }
# resolve NAME DIR: as gcon resolves link NAME from a file in DIR
resolve() {
	for d in "$2" procs gprocs; do
		[ -f "$d/$1.icn" ] && { echo "$d/$1.icn"; return; }
	done
	echo "iplclosure.sh: link $1 (from $2) not found" >&2
}
seen=" "
queue="$*"
while [ -n "$queue" ]; do
	set -- $queue
	f=$1
	shift
	queue="$*"
	case "$seen" in *" $f "*) continue ;; esac
	seen="$seen$f "
	[ -f "$f" ] || continue
	echo "$f"
	dir=$(dirname "$f")
	for n in $(links "$f"); do
		[ -n "$n" ] && queue="$queue $(resolve "$n" "$dir")"
	done
	for i in $(incs "$f"); do
		for d in incl gincl "$dir"; do
			[ -f "$d/$i" ] && { queue="$queue $d/$i"; break; }
		done
	done
done | sed 's/\.icn$//' | sort
