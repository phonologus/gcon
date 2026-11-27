#!/bin/sh
# Tests the editor support: the grammar (tree-sitter-gcon), the Zed
# extension and the VS Code extension.
#
#   editors/test.sh            the tests that need no display
#   editors/test.sh -vscode    also VS Code's integration tests, which
#                              open VS Code (needs a display, and
#                              ../bin/gcon: run ../build.sh first)
#
# What it checks:
#   - the queries carry gcon's built-in functions (gen.sh has been run);
#   - with the tree-sitter CLI (on PATH, or tree-sitter-gcon's
#     node_modules: npm install there): src/ is generated from grammar.js,
#     the corpus tests pass, every query compiles against the grammar,
#     and of all the Icon files in Arizona Icon's tree (../../icon, or
#     $ICON_SRC) and gcon's tests and library, exactly those listed in
#     tree-sitter-gcon/failing.txt fail to parse (-update rewrites it);
#   - the versioned files agree (tools/release.sh -check);
#   - Zed's extension.toml pins the grammar as it is now, and (with cargo
#     and its wasm32-wasip2 target) the extension builds;
#   - (with vscode's node_modules: npm install there) the TextMate
#     grammar's unit tests pass.
# A part whose tools are missing is skipped, and said to be.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
vscode= update=
for a in "$@"; do
	case $a in
	-vscode) vscode=1 ;;
	-update) update=1 ;;
	*) echo "usage: editors/test.sh [-vscode] [-update]" >&2; exit 2 ;;
	esac
done
fail=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
skip() { echo "skip $*"; }

# gen.sh's output is current.
queries="tree-sitter-gcon/queries/highlights.scm zed/languages/icon/highlights.scm"
for q in $queries; do mkdir -p "$tmp/q/$(dirname "$q")"; cp "$here/$q" "$tmp/q/$q"; done
"$here/gen.sh"
for q in $queries; do
	if ! cmp -s "$here/$q" "$tmp/q/$q"; then
		cp "$tmp/q/$q" "$here/$q"
		bad "$q: built-in functions out of date: run editors/gen.sh"
	fi
done

# The grammar.
g=$here/tree-sitter-gcon
ts=$(command -v tree-sitter || true)
[ -z "$ts" ] && [ -x "$g/node_modules/.bin/tree-sitter" ] && ts=$g/node_modules/.bin/tree-sitter
if [ -z "$ts" ]; then
	skip "grammar: no tree-sitter CLI (npm install in editors/tree-sitter-gcon)"
else
	# the CLI warns without a configuration; give it an empty one
	echo '{"parser-directories": []}' >"$tmp/ts.json"
	tsq() { "$ts" "$@" --config-path "$tmp/ts.json"; }
	mkdir "$tmp/gen"
	cp -R "$g/grammar.js" "$g/package.json" "$g/tree-sitter.json" "$g/src" "$tmp/gen/"
	if (cd "$tmp/gen" && "$ts" generate >/dev/null 2>&1); then
		for f in parser.c grammar.json node-types.json; do
			cmp -s "$tmp/gen/src/$f" "$g/src/$f" || bad "grammar: src/$f is not generated from grammar.js (tree-sitter generate)"
		done
	else
		bad "grammar: tree-sitter generate fails"
	fi
	(cd "$g" && "$ts" test >"$tmp/test.out" 2>&1) || { bad "grammar: corpus tests"; sed 's/\x1b\[[0-9;]*m//g' "$tmp/test.out" | grep -E "✗|^Total"; }
	sample=$root/vm/tests/programs/lib/linka.icn
	for q in "$g"/queries/*.scm "$here"/zed/languages/icon/*.scm; do
		tsq query -q -p "$g" "$q" "$sample" >/dev/null 2>"$tmp/q.err" || { bad "query ${q#"$here"/} does not compile:"; grep -i error "$tmp/q.err"; }
	done
	# Every Icon file: those that fail to parse, as prefix:path.
	icon=${ICON_SRC:-$root/../icon}
	: >"$tmp/files"
	if [ -d "$icon/ipl" ]; then
		icon=$(cd "$icon" && pwd)
		find "$icon/ipl" "$icon/tests" -name '*.icn' >>"$tmp/files"
	else
		skip "grammar: Arizona Icon's tree ($icon): gcon's files only"
	fi
	find "$root/src/tests" "$root/vm/tests" "$root/lib/ipl" -name '*.icn' >>"$tmp/files"
	tsq parse -q -s -p "$g" --paths "$tmp/files" >"$tmp/parse.out" 2>&1
	grep -E '\((ERROR|MISSING)' "$tmp/parse.out" | awk '{ print $1 }' |
		sed "s|^$icon/|icon:|; s|^$root/|gcon:|" | LC_ALL=C sort >"$tmp/failing"
	n=$(wc -l <"$tmp/files" | tr -d ' ')
	if [ "$update" ]; then
		cp "$tmp/failing" "$g/failing.txt"
	elif ! cmp -s "$tmp/failing" "$g/failing.txt"; then
		bad "grammar: the files that fail to parse have changed (< listed, > now):"
		diff "$g/failing.txt" "$tmp/failing"
	fi
	echo "grammar: $n files parsed, $(wc -l <"$tmp/failing" | tr -d ' ') fail as listed"
fi

# Every versioned file says the same version, not older than the latest
# release (tools/release.sh keeps them so).
"$root/tools/release.sh" -check >"$tmp/release.out" 2>&1 || { bad "versions (tools/release.sh -check)"; cat "$tmp/release.out"; }

# Zed: the pinned grammar is the current one.
rev=$(sed -n 's/^rev = "\(.*\)"$/\1/p' "$here/zed/extension.toml")
if ! git -C "$root" cat-file -e "$rev^{commit}" 2>/dev/null; then
	bad "zed: extension.toml pins $rev, not a commit of this repository"
elif [ -n "$(git -C "$root" diff --name-only "$rev" -- editors/tree-sitter-gcon/grammar.js editors/tree-sitter-gcon/src)" ]; then
	bad "zed: the grammar has changed since the commit extension.toml pins ($rev): commit it, and pin that commit"
fi
if command -v cargo >/dev/null 2>&1 && rustup target list --installed 2>/dev/null | grep -qx wasm32-wasip2; then
	(cd "$here/zed" && cargo build --release --target wasm32-wasip2 >"$tmp/cargo.out" 2>&1) ||
		{ bad "zed: the extension does not build"; tail -20 "$tmp/cargo.out"; }
else
	skip "zed: no cargo with the wasm32-wasip2 target (rustup target add wasm32-wasip2)"
fi

# VS Code.
if [ -d "$here/vscode/node_modules" ]; then
	(cd "$here/vscode" && npm run -s test:unit >"$tmp/unit.out" 2>&1) || { bad "vscode: unit tests"; tail -30 "$tmp/unit.out"; }
	if [ "$vscode" ]; then
		(cd "$here/vscode" && node ./out/test/runTest.js >"$tmp/int.out" 2>&1) ||
			{ bad "vscode: integration tests"; grep -E "passing|failing|✔|[0-9]+\)" "$tmp/int.out" | tail -30; }
	fi
else
	skip "vscode: no node_modules (npm install in editors/vscode)"
fi

if [ "$fail" -eq 0 ]; then echo "editors: pass"; else echo "editors: $fail problems"; fi
[ "$fail" -eq 0 ]
