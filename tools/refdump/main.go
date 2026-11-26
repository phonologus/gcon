// Command refdump writes the Go reference's view of an Icon source file
// in a canonical text form, for the C front end's differential tests
// (src/tests/): the C writes the same form, and the two must be
// identical. It is test tooling for the frozen reference, and goes with
// it.
//
//	refdump lex FILE    tokens, trivia, decoded literals, lexical errors
//	refdump parse FILE  the syntax tree, field by field, with tokens as
//	                    indexes; the token walk; the errors
//	refdump walk FILE   the same without the tree
//	refdump pp FILE     the preprocessed text, line by line with the map,
//	                    and the errors ($PPPATH: the include path)
//	refdump predef      the names of the predefined symbols
package main

import (
	"fmt"
	"math"
	"os"
	"strings"

	"github.com/phonologus/gcon/internal/lexer"
	"github.com/phonologus/gcon/internal/lit"
	"github.com/phonologus/gcon/internal/token"
)

// esc writes s with bytes outside printable ASCII, and the backslash,
// as \xHH.
func esc(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < 0x20 || c >= 0x7f || c == '\\' {
			fmt.Fprintf(&b, "\\x%02x", c)
		} else {
			b.WriteByte(c)
		}
	}
	return b.String()
}

func lex(src string) {
	toks, errs := lexer.Tokenize(src)
	var out strings.Builder
	for _, t := range toks {
		fmt.Fprintf(&out, "T %s %d %d", esc(t.Kind.String()), t.Offset, len(t.Text))
		if t.Implicit {
			out.WriteString(" implicit")
		}
		out.WriteByte('\n')
		for _, tv := range t.Leading {
			fmt.Fprintf(&out, "  t %s %d %s\n", tv.Kind, tv.Offset, esc(tv.Text))
		}
		fmt.Fprintf(&out, "  text %s\n", esc(t.Text))
		switch t.Kind {
		case token.STRINGLIT, token.CSETLIT:
			if s, u, err := lit.String(t.Text); err != nil {
				fmt.Fprintf(&out, "  s err %s\n", esc(err.Error()))
			} else {
				fmt.Fprintf(&out, "  s %s %s\n", u, esc(s))
			}
			if t.Kind == token.CSETLIT {
				if segs, rs, u, err := lit.Cset(t.Text); err != nil {
					fmt.Fprintf(&out, "  c err %s\n", esc(err.Error()))
				} else {
					fmt.Fprintf(&out, "  c %s %d\n", u, len(segs))
					for _, s := range segs {
						fmt.Fprintf(&out, "   seg %s\n", esc(s))
					}
					for _, r := range rs {
						fmt.Fprintf(&out, "   range %d %d %t\n", r.Lo, r.Hi, r.LoneBytes)
					}
				}
			}
		case token.INTLIT, token.REALLIT:
			n, err := lit.ParseNumber(t.Text)
			switch {
			case err != nil:
				fmt.Fprintf(&out, "  n err %s\n", esc(err.Error()))
			case n.IsReal:
				fmt.Fprintf(&out, "  n real %016x\n", math.Float64bits(n.Real))
			case n.Big != nil:
				fmt.Fprintf(&out, "  n big %s %x\n", n.Big.String(), n.Big.Bytes())
			default:
				fmt.Fprintf(&out, "  n int %d\n", n.Int)
			}
		}
	}
	for _, e := range errs {
		fmt.Fprintf(&out, "E %d %s\n", e.Offset, esc(e.Msg))
	}
	os.Stdout.WriteString(out.String())
}

func main() {
	if len(os.Args) == 2 && os.Args[1] == "predef" {
		predef()
		return
	}
	if len(os.Args) != 3 || (os.Args[1] != "lex" && os.Args[1] != "parse" && os.Args[1] != "walk" && os.Args[1] != "pp") {
		fmt.Fprintln(os.Stderr, "usage: refdump lex|parse|walk|pp FILE | predef")
		os.Exit(2)
	}
	b, err := os.ReadFile(os.Args[2])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if os.Args[1] == "pp" {
		pp(os.Args[2], string(b))
	} else if os.Args[1] != "lex" {
		parse(string(b), os.Args[1] == "parse")
	} else {
		lex(string(b))
	}
}
