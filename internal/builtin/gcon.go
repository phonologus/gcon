package builtin

// gcon's built-in functions and keywords that Icon 9.5 does not have
// (docs/design.md, "Strings and Unicode"). They are merged into the
// catalogue, marked Gcon.
var (
	gconFunctions = []Function{
		{Name: "bytes", Arity: 1, Signature: "bytes(x)", Summary: "relabel a string or cset as bytes", Gcon: true},
		{Name: "text", Arity: 1, Signature: "text(x)", Summary: "relabel a string or cset as text", Gcon: true},
		{Name: "uchar", Arity: 1, Signature: "uchar(i)", Summary: "produce the one-character text string for code point i", Gcon: true},
		{Name: "utf8valid", Arity: 1, Signature: "utf8valid(s)", Summary: "produce s if it is valid UTF-8, else fail", Gcon: true},
	}
	gconKeywords = []Keyword{
		{Name: "uset", Summary: "cset of every character of text: all code points, and the lone bytes", Gcon: true},
	}
)
