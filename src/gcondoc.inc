/* Hover documentation for gcon's additions to Icon 9.5, which have no
 * page in the Icon reference: Markdown, shown by gcon lsp in place of
 * the catalogue's one-line summary (builtin.inc). Each entry is checked
 * against gcon's behaviour; the design is in docs/design.md, "Strings
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
	{"utf8valid",
	 "```icon\nutf8valid(s)\n```\n\n"
	 "Produces `s` if it is valid UTF-8, and fails otherwise."},
};

/* The design notes, for the hover's footer. */
#define GCONDOC_URL "https://github.com/phonologus/gcon/blob/trunk/docs/design.md#strings-and-unicode"
