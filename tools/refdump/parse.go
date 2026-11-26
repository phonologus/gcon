package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/phonologus/gcon/internal/syntax"
)

// The parse form: one line per field, indented by depth. A token is
// written as its index in File.Tokens, an absent token or node as "-",
// a node as its type name with its fields below it, and a list as
// "list N" with each element and the separator after it. Then the
// order in which syntax.Tokens walks the file, and the errors.
type dumper struct {
	out   strings.Builder
	index map[*syntax.Token]int
}

func (d *dumper) line(depth int, field, val string) {
	d.out.WriteString(strings.Repeat(" ", depth))
	d.out.WriteString(field)
	d.out.WriteByte(' ')
	d.out.WriteString(val)
	d.out.WriteByte('\n')
}

func (d *dumper) tokstr(t *syntax.Token) string {
	if t == nil {
		return "-"
	}
	return fmt.Sprint(d.index[t])
}

func (d *dumper) tok(depth int, field string, t *syntax.Token) {
	d.line(depth, field, d.tokstr(t))
}

func (d *dumper) toks(depth int, field string, l syntax.List[*syntax.Token]) {
	d.line(depth, field, fmt.Sprintf("list %d %d", len(l.Elems), len(l.Seps)))
	for i, e := range l.Elems {
		d.tok(depth+1, "e", e)
		if i < len(l.Seps) {
			d.tok(depth+1, "s", l.Seps[i])
		}
	}
}

func (d *dumper) exprs(depth int, field string, l syntax.List[syntax.Expr]) {
	d.line(depth, field, fmt.Sprintf("list %d %d", len(l.Elems), len(l.Seps)))
	for i, e := range l.Elems {
		d.node(depth+1, "e", e)
		if i < len(l.Seps) {
			d.tok(depth+1, "s", l.Seps[i])
		}
	}
}

func (d *dumper) elems(depth int, es []any) {
	d.line(depth, "elems", fmt.Sprint(len(es)))
	for _, e := range es {
		switch e := e.(type) {
		case *syntax.Token:
			d.tok(depth+1, "t", e)
		case syntax.Node:
			d.node(depth+1, "n", e)
		default:
			panic(fmt.Sprintf("bad element %T", e))
		}
	}
}

func (d *dumper) node(depth int, field string, n syntax.Node) {
	if n == nil {
		d.line(depth, field, "-")
		return
	}
	name := strings.TrimPrefix(fmt.Sprintf("%T", n), "*syntax.")
	d.line(depth, field, name)
	k := depth + 1
	switch n := n.(type) {
	case *syntax.LinkDecl:
		d.tok(k, "Link", n.Link)
		d.toks(k, "Files", n.Files)
	case *syntax.GlobalDecl:
		d.tok(k, "Global", n.Global)
		d.toks(k, "Names", n.Names)
	case *syntax.RecordDecl:
		d.tok(k, "Record", n.Record)
		d.tok(k, "Name", n.Name)
		d.tok(k, "Lparen", n.Lparen)
		d.toks(k, "Fields", n.Fields)
		d.tok(k, "Rparen", n.Rparen)
	case *syntax.InvocableDecl:
		d.tok(k, "Invocable", n.Invocable)
		d.line(k, "Items", fmt.Sprintf("list %d %d", len(n.Items.Elems), len(n.Items.Seps)))
		for i, e := range n.Items.Elems {
			d.node(k+1, "e", e)
			if i < len(n.Items.Seps) {
				d.tok(k+1, "s", n.Items.Seps[i])
			}
		}
	case *syntax.InvocableItem:
		d.tok(k, "Name", n.Name)
		d.tok(k, "Colon", n.Colon)
		d.tok(k, "Arity", n.Arity)
	case *syntax.ProcDecl:
		d.tok(k, "Procedure", n.Procedure)
		d.tok(k, "Name", n.Name)
		d.tok(k, "Lparen", n.Lparen)
		d.toks(k, "Params", n.Params)
		d.tok(k, "Lbrack", n.Lbrack)
		d.tok(k, "Rbrack", n.Rbrack)
		d.tok(k, "Rparen", n.Rparen)
		d.tok(k, "Semi", n.Semi)
		d.line(k, "Locals", fmt.Sprint(len(n.Locals)))
		for _, l := range n.Locals {
			d.node(k+1, "l", l)
		}
		if n.Initial == nil {
			d.line(k, "Initial", "-")
		} else {
			d.node(k, "Initial", n.Initial)
		}
		d.exprs(k, "Body", n.Body)
		d.tok(k, "End", n.End)
	case *syntax.LocalDecl:
		d.tok(k, "Keyword", n.Keyword)
		d.toks(k, "Names", n.Names)
		d.tok(k, "Semi", n.Semi)
	case *syntax.InitialClause:
		d.tok(k, "Initial", n.Initial)
		d.node(k, "X", n.X)
		d.tok(k, "Semi", n.Semi)
	case *syntax.BadDecl:
		d.elems(k, n.Elems)
	case *syntax.Ident:
		d.tok(k, "Name", n.Name)
	case *syntax.Literal:
		d.tok(k, "Tok", n.Tok)
	case *syntax.Keyword:
		d.tok(k, "Amp", n.Amp)
		d.tok(k, "Name", n.Name)
	case *syntax.Unary:
		d.tok(k, "Op", n.Op)
		d.node(k, "X", n.X)
	case *syntax.Binary:
		d.node(k, "X", n.X)
		d.tok(k, "Op", n.Op)
		d.node(k, "Y", n.Y)
	case *syntax.ToBy:
		d.node(k, "X", n.X)
		d.tok(k, "To", n.To)
		d.node(k, "Y", n.Y)
		d.tok(k, "By", n.By)
		d.node(k, "Z", n.Z)
	case *syntax.Paren:
		d.tok(k, "Lparen", n.Lparen)
		d.exprs(k, "List", n.List)
		d.tok(k, "Rparen", n.Rparen)
	case *syntax.Block:
		d.tok(k, "Lbrace", n.Lbrace)
		d.exprs(k, "List", n.List)
		d.tok(k, "Rbrace", n.Rbrace)
	case *syntax.ListLit:
		d.tok(k, "Lbrack", n.Lbrack)
		d.exprs(k, "List", n.List)
		d.tok(k, "Rbrack", n.Rbrack)
	case *syntax.Index:
		d.node(k, "X", n.X)
		d.tok(k, "Lbrack", n.Lbrack)
		d.exprs(k, "List", n.List)
		d.tok(k, "Rbrack", n.Rbrack)
	case *syntax.Section:
		d.node(k, "X", n.X)
		d.tok(k, "Lbrack", n.Lbrack)
		d.node(k, "Lo", n.Lo)
		d.tok(k, "Op", n.Op)
		d.node(k, "Hi", n.Hi)
		d.tok(k, "Rbrack", n.Rbrack)
	case *syntax.Call:
		d.node(k, "Fun", n.Fun)
		d.tok(k, "Lparen", n.Lparen)
		d.exprs(k, "Args", n.Args)
		d.tok(k, "Rparen", n.Rparen)
	case *syntax.PDCO:
		d.node(k, "Fun", n.Fun)
		d.tok(k, "Lbrace", n.Lbrace)
		d.exprs(k, "Args", n.Args)
		d.tok(k, "Rbrace", n.Rbrace)
	case *syntax.Field:
		d.node(k, "X", n.X)
		d.tok(k, "Dot", n.Dot)
		d.tok(k, "Name", n.Name)
	case *syntax.Create:
		d.tok(k, "Create", n.Create)
		d.node(k, "X", n.X)
	case *syntax.If:
		d.tok(k, "If", n.If)
		d.node(k, "Cond", n.Cond)
		d.tok(k, "Then", n.Then)
		d.node(k, "Body", n.Body)
		d.tok(k, "Else", n.Else)
		d.node(k, "Alt", n.Alt)
	case *syntax.Loop:
		d.tok(k, "Keyword", n.Keyword)
		d.node(k, "Cond", n.Cond)
		d.tok(k, "Do", n.Do)
		d.node(k, "Body", n.Body)
	case *syntax.Return:
		d.tok(k, "Keyword", n.Keyword)
		d.node(k, "X", n.X)
		d.tok(k, "Do", n.Do)
		d.node(k, "Body", n.Body)
	case *syntax.Break:
		d.tok(k, "Break", n.Break)
		d.node(k, "X", n.X)
	case *syntax.Next:
		d.tok(k, "Next", n.Next)
	case *syntax.Case:
		d.tok(k, "Case", n.Case)
		d.node(k, "X", n.X)
		d.tok(k, "Of", n.Of)
		d.tok(k, "Lbrace", n.Lbrace)
		d.line(k, "Clauses", fmt.Sprintf("list %d %d", len(n.Clauses.Elems), len(n.Clauses.Seps)))
		for i, e := range n.Clauses.Elems {
			d.node(k+1, "e", e)
			if i < len(n.Clauses.Seps) {
				d.tok(k+1, "s", n.Clauses.Seps[i])
			}
		}
		d.tok(k, "Rbrace", n.Rbrace)
	case *syntax.CaseClause:
		d.tok(k, "Default", n.Default)
		d.node(k, "Label", n.Label)
		d.tok(k, "Colon", n.Colon)
		d.node(k, "Body", n.Body)
	case *syntax.BadExpr:
		d.elems(k, n.Elems)
	default:
		panic(fmt.Sprintf("unknown node %T", n))
	}
}

// parse writes the parse form; with tree false, only the token walk
// and the errors (the walk form, for deeply nested input).
func parse(src string, tree bool) {
	f := syntax.Parse(src)
	d := &dumper{index: map[*syntax.Token]int{}}
	for i := range f.Tokens {
		d.index[&f.Tokens[i]] = i
	}
	d.line(0, "File", fmt.Sprintf("%d tokens", len(f.Tokens)))
	if tree {
		for _, n := range f.Decls {
			d.node(1, "d", n)
		}
		d.tok(1, "EOF", f.EOF)
	}
	d.out.WriteString("W")
	for _, t := range syntax.Tokens(f) {
		d.out.WriteString(" " + d.tokstr(t))
	}
	d.out.WriteString("\n")
	for _, e := range f.Errors {
		fmt.Fprintf(&d.out, "E %d %s\n", e.Offset, esc(e.Msg))
	}
	os.Stdout.WriteString(d.out.String())
}
