/* Hover documentation for gcon's additions to Icon 9.5, which have no
 * page in the Icon reference: Markdown, shown by gcon lsp in place of
 * the catalogue's one-line summary (builtin.inc). Each entry is checked
 * against gcon's behaviour; the design is in docs/design.md, "Strings
 * and Unicode". Keywords are named with their "&". (The sources are
 * ASCII: \xc3\xa9 is "e" with an acute accent.) */

static const struct {
	const char *name;
	const char *md;
} gcondocs[] = {
	{"len",
	 "```icon\nlen(s)\n```\n\n"
	 "Produces the number of characters in `s`. A text string's characters are its UTF-8 code points "
	 "(each byte that is not part of valid UTF-8 counts as one character); a bytes string's are its bytes.\n\n"
	 "`*s` is the length in bytes, which is what positions and subscripts count: "
	 "`len(\"caf\xc3\xa9\")` is 4 and `*\"caf\xc3\xa9\"` is 5. For ASCII, and for bytes, the two agree.\n\n"
	 "`s` is converted to a string as for any string function: `len(123)` is 3."},
	{"uchar",
	 "```icon\nuchar(i)\n```\n\n"
	 "Produces the one-character text string for the code point `i`: `uchar(233)` is "
	 "`\"\xc3\xa9\"`. An invalid code point (a surrogate, or one above 0x10FFFF) gives U+FFFD.\n\n"
	 "`char(i)` keeps its Icon 9.5 meaning, a byte: `char(233)` is the bytes string `\"\\xe9\"`. "
	 "`ord` is the inverse of both."},
	{"runes",
	 "```icon\nrunes(x)\n```\n\n"
	 "Produces the string `x` relabelled as text: the same bytes, read as UTF-8 code points by the "
	 "operations that read characters (`!s`, `move`, `map`, `len`, ...). Positions and `*s` are unchanged. "
	 "It never fails and never copies; bytes that are not valid UTF-8 are kept, and each reads as one "
	 "character (U+FFFD to `ord`).\n\n"
	 "For a cset, `runes(c)` is `c`: csets have no unit."},
	{"bytes",
	 "```icon\nbytes(x)\n```\n\n"
	 "Produces the string `x` relabelled as bytes: the same bytes, each read as one character, as in "
	 "Icon 9.5. Positions and `*s` are unchanged; it never fails and never copies.\n\n"
	 "For a cset, produces the cset of the bytes that `c` matches in a bytes subject: each code point is "
	 "replaced by the bytes of its UTF-8 encoding, so `bytes('\xc3\xa9')` is `'\\xa9\\xc3'`."},
	{"utf8valid",
	 "```icon\nutf8valid(s)\n```\n\n"
	 "Produces `s` if it is valid UTF-8, and fails otherwise. Nothing else in gcon checks: text that is "
	 "not valid UTF-8 is kept as it is, its stray bytes reading as single characters."},
	{"&uset",
	 "```icon\n&uset\n```\n\n"
	 "The cset of every character: the 128 ASCII characters, the 128 non-ASCII bytes and every Unicode "
	 "code point (less the surrogates), 1,112,192 in all. `&uset` is `~''`.\n\n"
	 "`&cset` keeps its Icon 9.5 meaning, ASCII and the 128 bytes, so `~&cset` is every non-ASCII code "
	 "point, not the empty cset."},
};

/* The design notes, for the hover's footer. */
#define GCONDOC_URL "https://github.com/phonologus/gcon/blob/trunk/docs/design.md#strings-and-unicode"
