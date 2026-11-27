#!/bin/sh
# Releases gcon at a version Sean has chosen, by Go's rules: vX.Y.Z, a
# patch for fixes alone, a minor version for anything a program must
# adapt to (while X is 0). gcon's own version comes from the tag
# (build.sh stamps it); this script brings the files that carry a
# version of their own, the editor extensions and the grammar, to the
# same version, and makes the tag.
#
#   tools/release.sh vX.Y.Z NOTES   release (below); NOTES is the tag's message
#   tools/release.sh -check         the versioned files agree with each
#                                   other, and none is older than the
#                                   latest release tag (editors/test.sh
#                                   runs this)
#   tools/release.sh -set X.Y.Z     only rewrite the versioned files
#
# A release starts from a clean trunk whose editors/vscode/CHANGELOG.md
# already has a "## X.Y.Z" entry, and needs the tree-sitter CLI (npm
# install in editors/tree-sitter-gcon). It
#   1. writes X.Y.Z into every versioned file (the table below);
#   2. regenerates the grammar's src/ (tree-sitter generate copies the
#      version into parser.c), so the grammar has changed;
#   3. commits, pins that commit in editors/zed/extension.toml (as after
#      any grammar change), and commits the pin;
#   4. runs editors/test.sh (EDITORS_TEST_FLAGS, e.g. -vscode, are passed
#      on), stopping before the tag if it fails;
#   5. makes the annotated tag vX.Y.Z with NOTES as its message.
# It does not push: git push origin trunk vX.Y.Z.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

die() { echo "release.sh: $*" >&2; exit 1; }

# The versioned files: FILE|AFTER|LINE. The version is the first X.Y.Z
# on the first line matching LINE at or after the first line matching
# AFTER (from the top if AFTER is empty). Both are awk regular
# expressions; brackets stand in for backslashes.
table() {
	cat <<'EOF'
editors/vscode/package.json||^  "version": "
editors/vscode/package-lock.json||^  "version": "
editors/vscode/package-lock.json|^    "": [{]|^      "version": "
editors/tree-sitter-gcon/package.json||^  "version": "
editors/tree-sitter-gcon/package-lock.json||^  "version": "
editors/tree-sitter-gcon/package-lock.json|^    "": [{]|^      "version": "
editors/tree-sitter-gcon/tree-sitter.json|^  "metadata": [{]|^    "version": "
editors/zed/Cargo.toml|^[[]package[]]$|^version = "
editors/zed/Cargo.lock|^name = "zed_gcon"$|^version = "
editors/zed/extension.toml||^version = "
EOF
}
# and every gcon-X.Y.Z.vsix named in these:
vsixdocs="doc/editors.md editors/vscode/README.md"

# entry FILE AFTER LINE [NEW]: prints the entry's version, or with NEW
# rewrites it to NEW; fails if the entry is not found.
entry() {
	awk -v after="$2" -v line="$3" -v new="${4:-}" -v file="$1" '
		BEGIN { on = (after == ""); found = 0 }
		!on && $0 ~ after { on = 1 }
		on && !found && $0 ~ line && match($0, /[0-9]+\.[0-9]+\.[0-9]+/) {
			found = 1
			if (new == "") { print substr($0, RSTART, RLENGTH); exit }
			$0 = substr($0, 1, RSTART - 1) new substr($0, RSTART + RLENGTH)
		}
		new != "" { print }
		END { if (!found) { print "release.sh: " file ": no version at " line > "/dev/stderr"; exit 1 } }
	' "$1"
}

# versions prints FILE VERSION for every versioned place.
versions() {
	table | while IFS='|' read -r f after line; do
		v=$(entry "$f" "$after" "$line") || exit 1
		echo "$f $v"
	done || return 1
	for f in $vsixdocs; do
		grep -o 'gcon-[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.vsix' "$f" | sed "s|^gcon-\(.*\)\.vsix$|$f \1|"
	done
}

setversion() {
	table | while IFS='|' read -r f after line; do
		entry "$f" "$after" "$line" "$1" >"$f.new" || { rm -f "$f.new"; exit 1; }
		mv "$f.new" "$f"
	done || return 1
	for f in $vsixdocs; do
		sed "s/gcon-[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.vsix/gcon-$1.vsix/g" "$f" >"$f.new"
		mv "$f.new" "$f"
	done
}

# newer A B: true if version A (X.Y.Z) is after B.
newer() {
	awk -v a="$1" -v b="$2" 'BEGIN {
		split(a, x, "."); split(b, y, ".")
		for (i = 1; i <= 3; i++) if (x[i] + 0 != y[i] + 0) exit !(x[i] + 0 > y[i] + 0)
		exit 1
	}'
}

latest() { git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1; }

check() {
	v=$(versions) || exit 1
	n=$(echo "$v" | awk '{ print $2 }' | sort -u | wc -l | tr -d ' ')
	if [ "$n" != 1 ]; then
		echo "release.sh: the versioned files disagree:" >&2
		echo "$v" | sed 's/^/  /' >&2
		exit 1
	fi
	have=$(echo "$v" | awk 'NR == 1 { print $2 }')
	last=$(latest)
	if [ -n "$last" ] && newer "$last" "$have"; then
		die "the versioned files say $have, older than the release v$last: tools/release.sh -set $last"
	fi
	echo "release.sh: every versioned file says $have"
}

case ${1:-} in
-check)
	[ $# = 1 ] || die "usage: tools/release.sh -check"
	check
	exit 0 ;;
-set)
	[ $# = 2 ] && echo "$2" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || die "usage: tools/release.sh -set X.Y.Z"
	setversion "$2"
	check
	exit 0 ;;
v*) ;;
*) die "usage: tools/release.sh vX.Y.Z NOTES | -check | -set X.Y.Z" ;;
esac

# ---- a release ----
[ $# = 2 ] || die "usage: tools/release.sh vX.Y.Z NOTES"
tag=$1 notes=$2
echo "$tag" | grep -Eqx 'v[0-9]+\.[0-9]+\.[0-9]+' || die "$tag: a release is vX.Y.Z"
ver=${tag#v}
[ -s "$notes" ] || die "$notes: no notes for the tag's message"
notes=$(cd "$(dirname "$notes")" && pwd)/$(basename "$notes")
[ "$(git symbolic-ref --short HEAD 2>/dev/null)" = trunk ] || die "not on trunk"
[ -z "$(git status --porcelain)" ] || die "the tree has uncommitted changes"
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && die "$tag already exists"
prev=$(latest)
[ -z "$prev" ] || newer "$ver" "$prev" || die "$tag is not after the latest release, v$prev"
grep -qx "## $ver" editors/vscode/CHANGELOG.md || die "editors/vscode/CHANGELOG.md has no \"## $ver\" entry: write it and commit it first"
g=editors/tree-sitter-gcon
ts=$(command -v tree-sitter || true)
[ -z "$ts" ] && [ -x "$g/node_modules/.bin/tree-sitter" ] && ts=$root/$g/node_modules/.bin/tree-sitter
[ -n "$ts" ] || die "no tree-sitter CLI (npm install in $g)"

echo "release.sh: $tag (after v${prev:-none})"
setversion "$ver"

# The grammar, generated in a copy (as editors/test.sh checks it), so
# that only src/ changes.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$g/grammar.js" "$g/package.json" "$g/tree-sitter.json" "$g/src" "$tmp/"
(cd "$tmp" && "$ts" generate >/dev/null 2>&1) || die "tree-sitter generate fails"
for f in parser.c grammar.json node-types.json; do
	cmp -s "$tmp/src/$f" "$g/src/$f" || cp "$tmp/src/$f" "$g/src/$f"
done
check

files=$(table | cut -d'|' -f1 | sort -u)
# shellcheck disable=SC2086
git add $files $vsixdocs "$g/src"
git commit -q -m "$tag: the editor extensions and the grammar at $ver"
rev=$(git rev-parse HEAD)
sed "s/^rev = \"[0-9a-f]*\"$/rev = \"$rev\"/" editors/zed/extension.toml >editors/zed/extension.toml.new
mv editors/zed/extension.toml.new editors/zed/extension.toml
git add editors/zed/extension.toml
git commit -q -m "editors/zed: pin the grammar at $tag's commit"
echo "release.sh: committed $(git rev-parse --short HEAD~1) and $(git rev-parse --short HEAD)"

# shellcheck disable=SC2086
editors/test.sh ${EDITORS_TEST_FLAGS:-} || die "editors/test.sh fails: $tag not tagged (the two commits are made; fix, or git reset --hard HEAD~2)"
git tag -a "$tag" -F "$notes"
echo "release.sh: tagged $tag; push it with: git push origin trunk $tag"
