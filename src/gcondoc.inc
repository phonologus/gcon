/* Hover documentation for gcon's additions to Icon 9.5, which have no
 * page in the Icon reference: Markdown, shown by gcon lsp in place of
 * the catalogue's one-line summary (builtin.inc). Each entry is checked
 * against gcon's behaviour; the design is in design/design.md, "Strings
 * and Unicode". Strings are bytes, as in Icon 9.5; these read them as
 * UTF-8 where asked. (The sources are ASCII: \xc3\xa9 is "e" with an
 * acute accent.) */

static const struct {
	const char *name;
	const char *md;
} gcondocs[] = {
	{"uchar",
	 "```icon\nuchar(i)\n```\n\n"
	 "Produces the UTF-8 encoding of the code point `i`: `uchar(233)` is `\"\xc3\xa9\"`, two bytes. "
	 "An invalid code point (a surrogate, or one above 0x10FFFF) gives U+FFFD.\n\n"
	 "`char(i)` keeps its Icon 9.5 meaning, one byte: `char(233)` is `\"\\xe9\"`."},
	{"uord",
	 "```icon\nuord(s)\n```\n\n"
	 "Produces the code point of `s`, which must be one UTF-8 character (error 205 otherwise, as `ord`): "
	 "`uord(\"\xc3\xa9\")` is 233. A byte that is not part of valid UTF-8 gives U+FFFD (65533), as in Go; "
	 "`ord` gives its value."},
	{"uset",
	 "```icon\nuset(x)\n```\n\n"
	 "Converts `x` to a uset, a set of characters drawn from the ASCII characters, the 128 bytes from "
	 "0x80 and every Unicode code point. A string's characters are read as UTF-8 (a byte that is not part "
	 "of valid UTF-8 is that byte); a cset converts exactly. Fails if `x` has no string.\n\n"
	 "A uset is what reads UTF-8 in string scanning: given a uset, `any`, `many`, `upto`, `bal` and "
	 "`trim` step over whole UTF-8 characters, where with a cset they step over bytes. Positions are "
	 "always bytes. A uset literal is written `u'...'`, with ranges `u'a..z'`, and `~`, `++`, `--` and "
	 "`**` work on usets as on csets."},
	{"&uset",
	 "```icon\n&uset\n```\n\n"
	 "The uset of every character: the 128 ASCII characters, the 128 bytes from 0x80 and every Unicode "
	 "code point (less the surrogates). `tab(any(&uset))` is the next character of the subject, whole."},
	{"uvalid",
	 "```icon\nuvalid(s)\n```\n\n"
	 "Produces `s` if it is valid UTF-8, and fails otherwise."},
};

/* The design notes, for the hover's footer. */
#define GCONDOC_URL "https://github.com/phonologus/gcon/blob/trunk/design/design.md#strings-and-unicode"
