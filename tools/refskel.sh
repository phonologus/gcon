#!/bin/sh
# refskel.sh: skeletons of reference entries (doc/ref), from the facts in
# Icon 9.5's runtime sources (public domain): each function's synopsis
# and one-line summary, whether it generates, can fail or always
# produces one result, its defaults (def:, and the string-analysis
# defaults of str_anal) and its run-time errors (runerr), with the
# runtime's macros (#begdef) expanded and its parameters given the
# summary's names. A skeleton is a start, not an entry: check every fact
# against Gcon (an example in the entry does), and write the description
# in Gcon's own words. design/design.md, "The reference, and copyright".
#
#   tools/refskel.sh [-r runtime] name ...    # functions, by name
#   tools/refskel.sh [-r runtime] -all        # every function
#
# runtime defaults to ../icon/src/runtime beside the checkout.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
rt=$root/../icon/src/runtime
if [ "${1:-}" = -r ]; then
	rt=$2
	shift 2
fi
[ $# -gt 0 ] || { echo "usage: tools/refskel.sh [-r runtime] name ... | -all" >&2; exit 2; }
[ -d "$rt" ] || { echo "refskel.sh: no runtime sources at $rt" >&2; exit 1; }
cat "$rt"/*.r | awk -v want=" $* " '
function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
function params(s, a,   n, i) {
	sub(/^[^(]*\(/, "", s); sub(/\).*/, "", s)
	n = split(s, a, ",")
	for (i = 1; i <= n; i++) a[i] = trim(a[i])
	return n
}
# rename gives an implementation parameter its name in the summary.
function rename(x) { x = trim(x); return (x in docname) ? docname[x] : x }
function adddef(d,   i) { for (i = 1; i <= ndef; i++) if (defs[i] == d) return; defs[++ndef] = d }
function adderr(e,   i) { for (i = 1; i <= nerr; i++) if (errs[i] == e) return; errs[++nerr] = e }
# scan reads one line of a function body for defaults and errors.
function scan(s,   t, d, arg, val, e, num, p, n) {
	if (s ~ /str_anal\(/) {
		t = s; sub(/.*str_anal\(/, "", t); sub(/\).*/, "", t)
		n = split(t, p, ",")
		adddef("`" rename(p[1]) "` `&subject`")
		adddef("`" rename(p[2]) "` `&pos` if `" rename(p[1]) "` is defaulted, else 1")
		adddef("`" rename(p[3]) "` 0")
		adderr("103 (`" rename(p[1]) "`)")
		adderr("101 (`" rename(p[2]) "`, `" rename(p[3]) "`)")
		return
	}
	t = s
	while (match(t, /def:[A-Za-z_]+\([^,()]+,[^)]*\)/)) {
		d = substr(t, RSTART, RLENGTH); t = substr(t, RSTART + RLENGTH)
		sub(/^def:[A-Za-z_]+\(/, "", d); sub(/\)$/, "", d)
		arg = d; sub(/,.*/, "", arg); val = d; sub(/^[^,]*,/, "", val)
		adddef("`" rename(arg) "` " trim(val))
	}
	t = s
	while (match(t, /runerr\([0-9]+[^)]*\)/)) {
		e = substr(t, RSTART, RLENGTH); t = substr(t, RSTART + RLENGTH)
		sub(/^runerr\(/, "", e); sub(/\)$/, "", e)
		num = trim(e); sub(/,.*/, "", num)
		if (num == "0") continue
		arg = ""
		if (e ~ /,/) { arg = e; sub(/^[^,]*,/, "", arg); arg = rename(arg) }
		adderr(num (arg != "" ? " (`" arg "`)" : ""))
	}
}
function flush(   i, out) {
	if (name == "" || (want != " -all " && index(want, " " name " ") == 0)) return
	printf "## %s\n\n```\n%s\n```\n\n%s\n\n", name, sig, summary
	if (results == "*") print "Generates its results.\n"
	else if (results == "0,1") print "Produces one result, or fails.\n"
	else if (results == "1") print "Produces one result.\n"
	else if (results == "0") print "Fails.\n"
	else printf "Results: {%s}.\n\n", results
	if (ndef) { out = ""; for (i = 1; i <= ndef; i++) out = out (i > 1 ? "; " : "") defs[i]; printf "**Defaults:** %s.\n\n", out }
	if (nerr) { out = ""; for (i = 1; i <= nerr; i++) out = out (i > 1 ? "; " : "") errs[i]; printf "**Errors:** %s.\n\n", out }
	print "**See also:**\n"
}
{ line[NR] = $0 }
END {
	# the macros, with their parameters
	for (i = 1; i <= NR; i++) {
		if (line[i] !~ /^#begdef[ \t]/) continue
		m = line[i]; sub(/^#begdef[ \t]+/, "", m)
		mname = m; sub(/\(.*/, "", mname); mname = trim(mname)
		mnp[mname] = (m ~ /\(/) ? params(m, mp) : 0
		for (k = 1; k <= mnp[mname]; k++) mparam[mname, k] = mp[k]
		body = ""
		for (j = i + 1; j <= NR && line[j] !~ /^#enddef/; j++) body = body line[j] "\n"
		macro[mname] = body
		i = j
	}
	doc = ""
	for (i = 1; i <= NR; i++) {
		s = line[i]
		if (!inbody && s ~ /^"[A-Za-z]+\(/) {
			doc = ""
			while (i <= NR && line[i] ~ /^"/) {
				t = line[i]; sub(/^"/, "", t); sub(/"[ \t]*$/, "", t)
				doc = doc t
				i++
			}
			s = line[i]
		}
		if (s ~ /^function\{/) {
			flush()
			name = s; sub(/^function\{[^}]*\}[ \t]*/, "", name); sub(/\(.*/, "", name)
			results = s; sub(/^function\{/, "", results); sub(/\}.*/, "", results)
			sig = doc; sub(/ - .*/, "", sig)
			summary = doc; sub(/^[^-]* - /, "", summary)
			if (sig == "" || index(sig, name "(") != 1) { sig = name "(...)"; summary = "(No summary in the runtime.)" }
			summary = toupper(substr(summary, 1, 1)) substr(summary, 2)
			for (x in docname) delete docname[x]
			ni = params(s, impl); nd = params(sig, dp)
			for (k = 1; k <= ni && k <= nd; k++) if (impl[k] != dp[k]) docname[impl[k]] = dp[k]
			ndef = 0; nerr = 0; inbody = 1; doc = ""
			continue
		}
		if (!inbody) continue
		if (s ~ /^end[ \t]*$/) { inbody = 0; continue }
		scan(s)
		# a macro used here: its body too, its arguments in place of its parameters
		for (mname in macro) {
			if (mname == "str_anal" || !match(s, "(^|[^A-Za-z0-9_])" mname "([^A-Za-z0-9_]|$)")) continue
			body = macro[mname]
			if (mnp[mname]) {
				t = substr(s, index(s, mname)); n = params(t, ma)
				for (k = 1; k <= mnp[mname] && k <= n; k++) gsub("(^|[^A-Za-z0-9_])" mparam[mname, k] "([^A-Za-z0-9_]|$)", " " ma[k] " ", body)
			}
			nb = split(body, bl, "\n")
			for (k = 1; k <= nb; k++) scan(bl[k])
		}
	}
	flush()
}'
