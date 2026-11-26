#!/bin/sh
# Install gcon-vm into a gcon root, where gcon run finds it:
# ROOT/libexec/gcon-vm. ROOT defaults to $GCONROOT, else this checkout
# (a checkout of gcon is a root; ../build.sh installs it there too).
# Builds first if needed.
#
#   tools/install.sh [root]
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=${1:-${GCONROOT:-$here/..}}
root=$(cd "$root" && pwd)
[ -d "$root/lib/ipl/procs" ] || { echo "install.sh: $root is not a gcon root" >&2; exit 2; }
[ -x "$here/bin/gcon-vm" ] || "$here/build.sh"
mkdir -p "$root/libexec"
cp "$here/bin/gcon-vm" "$root/libexec/gcon-vm.new"
mv "$root/libexec/gcon-vm.new" "$root/libexec/gcon-vm"
echo "$root/libexec/gcon-vm"
