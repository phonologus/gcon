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
	{"len",
	 "```icon\nlen(s)\n```\n\n"
	 "Produces the number of characters in `s` read as UTF-8: each valid sequence counts as one, and so "
	 "does each byte that is not part of one.\n\n"
	 "`*s` is the length in bytes, which is what positions and subscripts count: "
	 "`len(\"caf\xc3\xa9\")` is 4 and `*\"caf\xc3\xa9\"` is 5. For ASCII the two agree.\n\n"
	 "`s` is converted to a string as for any string function: `len(123)` is 3."},
	{"uchar",
	 "```icon\nuchar(i)\n```\n\n"
	 "Produces the UTF-8 encoding of the code point `i`: `uchar(233)` is `\"\xc3\xa9\"`, two bytes. "
	 "An invalid code point (a surrogate, or one above 0x10FFFF) gives U+FFFD.\n\n"
	 "`char(i)` keeps its Icon 9.5 meaning, one byte: `char(233)` is `\"\\xe9\"`."},
	{"utf8valid",
	 "```icon\nutf8valid(s)\n```\n\n"
	 "Produces `s` if it is valid UTF-8, and fails otherwise."},
	{"readrunes",
	 "```icon\nreadrunes(f,i)\n```\n\n"
	 "Reads `i` UTF-8 characters from file `f` (default `&input`, `i` defaults to 1), as `reads` reads `i` "
	 "bytes. A sequence is never split; each byte that is not part of valid UTF-8 counts as one character. "
	 "Fails at end of file; at the end it produces what is left.\n\n"
	 "`reads` counts bytes, as Icon 9.5's low-level reader, so it reads binary data and agrees with "
	 "`seek` and `where`."},
};

/* The design notes, for the hover's footer. */
#define GCONDOC_URL "https://github.com/phonologus/gcon/blob/trunk/docs/design.md#strings-and-unicode"
