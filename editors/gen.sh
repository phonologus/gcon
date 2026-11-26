#!/bin/sh
# Writes gcon's built-in function names, from its catalogue
# (../src/builtin.inc), into the editors' highlight queries, between the
# lines "; gen.sh: built-in functions" and "; gen.sh: end". Run it when
# the catalogue changes; test.sh checks that it has been.
#
#   editors/gen.sh
#
# (The VS Code extension's TextMate grammar takes the same names, when it
# is compiled: vscode/scripts/gen-grammar.js.)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
names=$(awk '/^static const BuiltinFunc functions\[\] = \{/ { on = 1; next }
	on && /^\};/ { exit }
	on { split($0, f, "\""); printf "%s\"%s\"", (n++ ? " " : ""), f[2] }' "$here/../src/builtin.inc")
[ -n "$names" ] || { echo "gen.sh: no built-in functions in ../src/builtin.inc" >&2; exit 1; }
for q in tree-sitter-gcon/queries/highlights.scm zed/languages/icon/highlights.scm; do
	f=$here/$q
	awk -v names="$names" '
		/^; gen\.sh: built-in functions/ { print; print "((call_expression function: (identifier) @function.builtin)"; print "  (#any-of? @function.builtin " names "))"; skip = 1; next }
		/^; gen\.sh: end/ { skip = 0 }
		!skip { print }' "$f" >"$f.new"
	mv "$f.new" "$f"
done
