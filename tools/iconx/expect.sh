#!/bin/sh
# Generate the expected output of gcon's test programs from the reference
# implementation: for each FILE.icn, FILE.std holds its standard output and,
# if it exits with a nonzero status, FILE.exit holds the status. If FILE.icn
# has a line "#STDERR", its standard error is recorded too, in FILE.err. Standard
# input comes from FILE.dat and arguments from FILE.args (separated by
# white space), if they exist.
#
#   tools/iconx/expect.sh FILE.icn...
#
# Uses tools/iconx/build.sh to build icont and iconx if needed.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=$("$here/build.sh")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in "$@"; do
  base=${f%.icn}
  name=$(basename "$base")
  cp "$f" "$tmp/$name.icn"
  (cd "$tmp" && "$bin/icont" -s -o "$name" "$name.icn")
  in=/dev/null
  [ -f "$base.dat" ] && in=$base.dat
  args=
  [ -f "$base.args" ] && args=$(cat "$base.args")
  set +e
  err=/dev/null
  grep -q '^#STDERR' "$f" && err=$base.err
  # shellcheck disable=SC2086
  "$tmp/$name" $args <"$in" >"$base.std" 2>"$err"
  status=$?
  set -e
  rm -f "$base.exit"
  [ $status -ne 0 ] && echo $status >"$base.exit"
  echo "$f: status $status"
done
