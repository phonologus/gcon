package main

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"github.com/phonologus/gcon/internal/ipp"
	"github.com/phonologus/gcon/internal/run"
	"github.com/phonologus/gcon/internal/sema"
	"github.com/phonologus/gcon/internal/syntax"
)

// The sema form: the files given are one program, resolved twice: as
// the compiler sees them (preprocessed, with gcon's predefined symbols
// and the include path in $PPPATH) and as an editor does (raw, so that
// predefined symbols are preprocessor symbols). For each file of each:
// the diagnostics, with the symbol each relates to; for each identifier
// token (by index), its symbol; the field and keyword uses; and each
// procedure's locals, by name. A symbol is written as its kind, name
// and declaring file and token: "kind name f:t".
func semaDump(names []string) {
	var out strings.Builder
	for _, pre := range []bool{true, false} {
		files := make([]*sema.File, len(names))
		for i, name := range names {
			b, err := os.ReadFile(name)
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
			src := string(b)
			if pre {
				src = ipp.Preprocess(name, src, ipp.Options{
					Predefined:  run.Predefined(),
					IncludePath: strings.Fields(os.Getenv("PPPATH")),
				}).Src
			}
			files[i] = &sema.File{Path: name, Syntax: syntax.Parse(src), Preprocessed: pre}
		}
		p := sema.NewProgram(files, true)
		fileIndex := map[*sema.File]int{}
		for i, f := range files {
			fileIndex[f] = i
		}
		sym := func(s *sema.Symbol) string {
			if s == nil {
				return "-"
			}
			where := "-"
			if s.File != nil && s.Decl != nil {
				where = fmt.Sprintf("%d:%d", fileIndex[s.File], tokIndex(s.File.Syntax, s.Decl))
			}
			return fmt.Sprintf("%s %s %s", strings.ReplaceAll(s.Kind.String(), " ", "_"), s.Name, where)
		}
		fmt.Fprintf(&out, "P preprocessed=%t\n", pre)
		for i, f := range files {
			info := p.Resolve(f)
			fmt.Fprintf(&out, "F %d %s\n", i, esc(f.Path))
			for _, d := range info.Diagnostics {
				fmt.Fprintf(&out, "D %d %d %d %s R %s\n", d.Offset, d.End, d.Severity, esc(d.Msg), sym(d.Related))
			}
			toks := f.Syntax.Tokens
			for k := range toks {
				t := &toks[k]
				if s, ok := info.Uses[t]; ok {
					fmt.Fprintf(&out, "U %d %s\n", k, sym(s))
				}
				if fs, ok := info.FieldUses[t]; ok {
					fmt.Fprintf(&out, "FU %d %d", k, len(fs))
					for _, s := range fs {
						fmt.Fprintf(&out, " [%s]", sym(s))
					}
					out.WriteByte('\n')
				}
				if kw, ok := info.Keywords[t]; ok {
					name := "-"
					if kw != nil {
						name = kw.Name
					}
					fmt.Fprintf(&out, "K %d %s\n", k, name)
				}
			}
			for _, d := range f.Syntax.Decls {
				pd, ok := d.(*syntax.ProcDecl)
				if !ok {
					continue
				}
				locals := info.Locals[pd]
				var ns []string
				for n := range locals {
					ns = append(ns, n)
				}
				sort.Strings(ns)
				fmt.Fprintf(&out, "L %d", tokIndex(f.Syntax, pd.Procedure))
				for _, n := range ns {
					fmt.Fprintf(&out, " [%s]", sym(locals[n]))
				}
				out.WriteByte('\n')
			}
		}
	}
	os.Stdout.WriteString(out.String())
}

func tokIndex(f *syntax.File, t *syntax.Token) int {
	for i := range f.Tokens {
		if &f.Tokens[i] == t {
			return i
		}
	}
	return -1
}
