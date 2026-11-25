package builtin

// gcon's built-in functions and keywords that Icon 9.5 does not have
// (docs/design.md, "Strings and Unicode"). They are merged into the
// catalogue, marked Gcon.
var (
	gconFunctions = []Function{
		{Name: "bytes", Arity: 1, Signature: "bytes(x)", Summary: "relabel a string as bytes; or the bytes matching a cset", Gcon: true},
		{Name: "len", Arity: 1, Signature: "len(s)", Summary: "produce the number of characters in s (*s is its length in bytes)", Gcon: true},
		{Name: "runes", Arity: 1, Signature: "runes(x)", Summary: "relabel a string as text (UTF-8, read as runes)", Gcon: true},
		{Name: "uchar", Arity: 1, Signature: "uchar(i)", Summary: "produce the one-character text string for code point i", Gcon: true},
		{Name: "utf8valid", Arity: 1, Signature: "utf8valid(s)", Summary: "produce s if it is valid UTF-8, else fail", Gcon: true},
	}
	gconKeywords = []Keyword{
		{Name: "uset", Summary: "cset of every character: ASCII, the non-ASCII bytes and all code points", Gcon: true},
	}
)
