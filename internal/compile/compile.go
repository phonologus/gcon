// Package compile translates resolved Icon syntax trees to VM code.
//
// Goal-directed evaluation is compiled with the translation of Todd
// Proebsting, "Simple Translation of Goal-Directed Evaluation" (PLDI
// 1997), as in Jcon and Goaldi. Each expression has a start label, where
// control enters to produce its first value, and a resume label, where
// control enters to produce its next value; the expression is given the
// labels to go to when it succeeds (its value then being in its result
// operand) and when it fails. Within a procedure, backtracking is
// therefore just jumps.
//
// Expressions that need no code, such as literals and variables, use
// their success label as their start label and their failure label as
// their resume label. Labels may be bound to other labels (aliases),
// which are resolved when the procedure is finished.
package compile

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"github.com/phonologus/gcon/internal/lexer"
	"github.com/phonologus/gcon/internal/lit"
	"github.com/phonologus/gcon/internal/sema"
	"github.com/phonologus/gcon/internal/syntax"
	"github.com/phonologus/gcon/internal/token"
	"github.com/phonologus/gcon/internal/vm"
)

// Error is a compilation error.
type Error struct {
	File string
	Line int
	Msg  string
}

func (e *Error) Error() string { return fmt.Sprintf("File %s; Line %d # %s", e.File, e.Line, e.Msg) }

// compiler holds program-wide state.
type compiler struct {
	prog    *vm.Program
	globals map[string]int         // global slots by name
	consts  map[string]int         // constant pool indices, by a key for the value
	fields  map[string]*vm.Builtin // field reference operators, by name
	errs    []error
	// For string invocation (see invocable): the globals each procedure
	// refers to, the names declared invocable, and invocable all.
	refs     map[string]map[string]bool
	invoc    []string
	invocAll bool
}

// Compile compiles a program whose files have been resolved.
func Compile(p *sema.Program, infos []*sema.Info) (*vm.Program, []error) {
	c := &compiler{
		prog:    &vm.Program{Main: -1},
		globals: map[string]int{},
		consts:  map[string]int{},
		fields:  map[string]*vm.Builtin{},
		refs:    map[string]map[string]bool{},
	}
	// Globals, procedures and records, in name order.
	names := make([]string, 0, len(p.Globals))
	for n := range p.Globals {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		c.global(n, vm.NullValue)
	}
	type procInfo struct {
		decl *syntax.ProcDecl
		info *sema.Info
	}
	var procs []procInfo
	for _, info := range infos {
		loc := locator(info.File)
		for _, d := range info.File.Syntax.Decls {
			switch d := d.(type) {
			case *syntax.ProcDecl:
				procs = append(procs, procInfo{d, info})
			case *syntax.GlobalDecl:
			case *syntax.RecordDecl:
				if d.Name == nil {
					continue
				}
				var fields []string
				for _, t := range d.Fields.Elems {
					if t != nil {
						fields = append(fields, t.Text)
					}
				}
				c.prog.Globals[c.global(d.Name.Text, vm.NullValue)] = vm.V(vm.NewRecordType(d.Name.Text, fields))
			case *syntax.LinkDecl:
				// Resolved by run.TranslateLinked, which passes the
				// linked files in.
			case *syntax.InvocableDecl:
				for _, it := range d.Items.Elems {
					if it == nil || it.Name == nil {
						continue
					}
					name := it.Name.Text
					if it.Name.Kind == token.STRINGLIT {
						name, _, _ = lit.String(name)
					}
					if it.Name.Kind == token.IDENT {
						// As in icont (addinvk), the only identifier
						// allowed is all.
						if name != "all" {
							file, line := loc(it.Name.Offset)
							c.errorf(file, line, "%q: invalid operand to invocable", name)
						}
						c.invocAll = true
					}
					c.invoc = append(c.invoc, name)
				}
			default:
				if s, _, ok := syntax.Span(d); ok {
					file, line := loc(s)
					c.errorf(file, line, "invalid declaration")
				}
			}
		}
	}
	for _, pi := range procs {
		if pi.decl.Name == nil {
			continue
		}
		proc := c.proc(pi.decl, pi.info)
		c.prog.Globals[c.globals[proc.Name]] = vm.V(proc)
	}
	if i, ok := c.globals["main"]; ok {
		if _, isProc := c.prog.Globals[i].Any().(*vm.Proc); isProc {
			c.prog.Main = i
		}
	}
	c.invocable()
	return c.prog, c.errs
}

// invocable records which globals string invocation can find. icont's
// linker discards the globals that main does not reach, directly or
// through the procedures it refers to, unless they are declared
// invocable (lglob.c, scanrefs); "invocable all" (or icont -fs) keeps
// them all. So "p"(x) is error 106 for a procedure p that is only ever
// named in strings.
func (c *compiler) invocable() {
	if c.invocAll {
		return
	}
	keep := make([]bool, len(c.prog.Globals))
	var mark func(name string)
	mark = func(name string) {
		i, ok := c.globals[name]
		if !ok || keep[i] {
			return
		}
		keep[i] = true
		for r := range c.refs[name] {
			mark(r)
		}
	}
	mark("main")
	for _, n := range c.invoc {
		mark(n)
	}
	c.prog.Invocable = keep
}

func (c *compiler) errorf(file string, line int, format string, args ...any) {
	c.errs = append(c.errs, &Error{File: file, Line: line, Msg: fmt.Sprintf(format, args...)})
}

// global returns the slot of the named global, creating it with value v.
func (c *compiler) global(name string, v vm.Value) int {
	if i, ok := c.globals[name]; ok {
		return i
	}
	i := len(c.prog.Globals)
	c.globals[name] = i
	c.prog.Globals = append(c.prog.Globals, v)
	c.prog.GlobalNames = append(c.prog.GlobalNames, name)
	return i
}

// constant returns the pool index of a constant, sharing equal ones.
func (c *compiler) constant(key string, v vm.Value) vm.Operand {
	i, ok := c.consts[key]
	if !ok {
		i = len(c.prog.Consts)
		c.consts[key] = i
		c.prog.Consts = append(c.prog.Consts, v)
	}
	return vm.Operand{Kind: vm.ConstOp, Index: int32(i)}
}

// ---------------------------------------------------------------------
// Procedures

type label int32

// loop is the context of break and next.
type loop struct {
	next   label      // where next goes
	fail   label      // where the loop goes when it fails
	succ   label      // where the loop goes when break succeeds
	result vm.Operand // the loop's result, set by break
	gate   vm.Operand // resume label of the break expression that produced the result
	resume label      // where resuming the loop goes
	broken bool       // the loop has a break
	scans  int        // depth of enclosing scans at the loop
}

// pcomp compiles one procedure.
type pcomp struct {
	c      *compiler
	info   *sema.Info
	file   string // the file of the code being compiled (see emit)
	loc    func(off int) (file string, line int)
	proc   *vm.Proc
	decl   *syntax.ProcDecl
	code   []vm.Instr
	pos    []int   // label -> code index, or -1
	alias  []label // label -> label it is bound to, or -1
	ntemps int
	locals map[*sema.Symbol]int
	loops  []*loop
	pfail  label        // the procedure's failure
	inCo   int          // depth of create expressions being compiled
	scans  []vm.Operand // environments of the scans enclosing the code being compiled
	line   int32
}

func (c *compiler) proc(d *syntax.ProcDecl, info *sema.Info) *vm.Proc {
	p := &pcomp{
		c:      c,
		info:   info,
		file:   info.File.Path,
		loc:    locator(info.File),
		decl:   d,
		locals: map[*sema.Symbol]int{},
	}
	proc := &vm.Proc{Name: d.Name.Text, Variadic: d.Lbrack != nil}
	p.proc = proc
	p.setLine(d.Name)
	proc.File = p.file

	// Parameters first, then the other locals in name order; statics
	// live in globals.
	for _, t := range d.Params.Elems {
		if s := info.Uses[t]; s != nil {
			p.locals[s] = len(p.locals)
		}
	}
	proc.NParams = len(p.locals)
	// Locals and statics in the order of their declarations (for an
	// implicit local, its first use), the order in which iconx's
	// display() lists them.
	syms := info.Locals[d]
	names := make([]string, 0, len(syms))
	for n := range syms {
		names = append(names, n)
	}
	sort.Slice(names, func(i, j int) bool {
		a, b := syms[names[i]], syms[names[j]]
		if a.Decl != nil && b.Decl != nil && a.Decl.Offset != b.Decl.Offset {
			return a.Decl.Offset < b.Decl.Offset
		}
		return names[i] < names[j]
	})
	for _, n := range names {
		s := syms[n]
		if _, ok := p.locals[s]; ok {
			continue
		}
		switch s.Kind {
		case sema.Static:
			proc.Statics = append(proc.Statics, c.global(proc.Name+" "+n, vm.NullValue))
		case sema.Local, sema.Implicit:
			p.locals[s] = len(p.locals)
		}
	}
	proc.NLocals = len(p.locals)
	proc.LocalNames = make([]string, proc.NLocals)
	for s, i := range p.locals {
		proc.LocalNames[i] = s.Name
	}

	p.pfail = p.newLabel()
	entry := p.newLabel()
	p.place(entry)
	if d.Initial != nil {
		// initial: run once, on the first call.
		flag := c.global(proc.Name+" initial", vm.NullValue)
		after := p.newLabel()
		p.emit(vm.Instr{Op: vm.OpOnce, X: vm.Operand{Kind: vm.GlobalOp, Index: int32(flag)}, L1: int32(after)})
		p.stmt(d.Initial.X, after)
		p.place(after)
	}
	p.stmts(d.Body.Elems, p.pfail)
	p.place(p.pfail)
	p.setLine(d.End) // falling off the end fails at "end", for &trace
	p.emit(vm.Instr{Op: vm.OpFail})

	proc.Code = p.link()
	proc.NTemps = p.ntemps
	return proc
}

func (p *pcomp) errorf(n syntax.Node, format string, args ...any) {
	file, line := p.file, int(p.line)
	if s, _, ok := syntax.Span(n); ok {
		file, line = p.loc(s)
	}
	p.c.errorf(file, line, format, args...)
}

func (p *pcomp) setLine(t *syntax.Token) {
	if t != nil {
		p.setPos(t.Offset)
	}
}

// setPos makes the source position of offset off the one instructions
// are emitted with.
func (p *pcomp) setPos(off int) {
	f, l := p.loc(off)
	p.file, p.line = f, int32(l)
}

// locator returns the position function for f: the preprocessor's map,
// or the lines of f itself.
func locator(f *sema.File) func(int) (string, int) {
	if f.Locate != nil {
		return f.Locate
	}
	lines := lexer.NewLines(f.Syntax.Src)
	return func(off int) (string, int) {
		l, _ := lines.Position(off)
		return f.Path, l
	}
}

func (p *pcomp) newLabel() label {
	p.pos = append(p.pos, -1)
	p.alias = append(p.alias, -1)
	return label(len(p.pos) - 1)
}

// place puts label l at the next instruction.
func (p *pcomp) place(l label) { p.pos[l] = len(p.code) }

// bind makes label a an alias for label b.
func (p *pcomp) bind(a, b label) {
	if a != b {
		p.alias[a] = b
	}
}

func (p *pcomp) emit(in vm.Instr) {
	in.Line = p.line
	if p.file != p.proc.File && p.proc.File != "" {
		// Code from another file (an $include within the procedure).
		k := 0
		for i, f := range p.proc.Files {
			if f == p.file {
				k = i + 1
			}
		}
		if k == 0 {
			p.proc.Files = append(p.proc.Files, p.file)
			k = len(p.proc.Files)
		}
		in.File = int32(k)
	}
	p.code = append(p.code, in)
}

func (p *pcomp) jump(l label) { p.emit(vm.Instr{Op: vm.OpGoto, L1: int32(l)}) }

func (p *pcomp) temp() vm.Operand {
	p.ntemps++
	return vm.Operand{Kind: vm.TempOp, Index: int32(p.ntemps - 1)}
}

// resolve returns the code index of label l, following aliases and
// jumps to jumps.
func (p *pcomp) resolve(l label) int32 {
	for n := 0; ; n++ {
		if p.alias[l] >= 0 && n < len(p.alias) {
			l = p.alias[l]
			continue
		}
		break
	}
	i := p.pos[l]
	if i < 0 {
		panic(fmt.Sprintf("compile: label %d not placed", l))
	}
	for n := 0; n < 8 && p.code[i].Op == vm.OpGoto; n++ {
		j := p.resolveNoThread(label(p.code[i].L1))
		if j == i {
			break
		}
		i = j
	}
	return int32(i)
}

func (p *pcomp) resolveNoThread(l label) int {
	for p.alias[l] >= 0 {
		l = p.alias[l]
	}
	return p.pos[l]
}

// link resolves labels to code indices.
func (p *pcomp) link() []vm.Instr {
	code := make([]vm.Instr, len(p.code))
	copy(code, p.code)
	for i := range code {
		in := &code[i]
		switch in.Op {
		case vm.OpGoto, vm.OpSetLabel, vm.OpSuspend, vm.OpOnce, vm.OpCreate, vm.OpCoRet:
			in.L1 = p.resolve(label(in.L1))
		case vm.OpOper, vm.OpCall, vm.OpResume, vm.OpIsNull, vm.OpActivate, vm.OpAssign:
			in.L1 = p.resolve(label(in.L1))
			in.L2 = p.resolve(label(in.L2))
		case vm.OpScanEnter:
			in.L2 = p.resolve(label(in.L2))
		}
	}
	return code
}

// stmts compiles a sequence of bounded expressions, then goes to next.
func (p *pcomp) stmts(es []syntax.Expr, next label) {
	for _, e := range es {
		after := p.newLabel()
		p.stmt(e, after)
		p.place(after)
	}
	p.jump(next)
}

// stmt compiles e as a bounded expression, entered from the current
// position: whether it succeeds or fails, control goes to next, and it is
// never resumed.
func (p *pcomp) stmt(e syntax.Expr, next label) {
	p.enter(func() label {
		start, _, _ := p.expr(e, next, next)
		return start
	})
}

// enter emits a jump from the current position to the start of the code
// that compile emits. Compiled code is never entered by falling into it:
// an expression's first instruction need not be its start, and every
// expression's code ends with a transfer of control.
func (p *pcomp) enter(compile func() label) {
	entry := p.newLabel()
	p.jump(entry)
	p.bind(entry, compile())
}

// seq compiles es left to right as a chain: the failure of each resumes
// the one before it, and the first fails to fail. Control goes to succ
// when all have produced values. It returns the chain's start, the
// resume label of the last expression, and the operands of the values.
func (p *pcomp) seq(es []syntax.Expr, fail, succ label) (start, resume label, vs []vm.Operand) {
	if len(es) == 0 {
		return succ, fail, nil
	}
	resume = fail
	var prevSucc label
	for i, e := range es {
		s := p.newLabel()
		st, r, v := p.expr(e, resume, s)
		if i == 0 {
			start = st
		} else {
			p.bind(prevSucc, st)
		}
		prevSucc, resume = s, r
		vs = append(vs, v)
	}
	p.bind(prevSucc, succ)
	return start, resume, vs
}

func (p *pcomp) null() vm.Operand { return p.c.constant("n", vm.NullValue) }

// expr compiles e. See the package comment.
func (p *pcomp) expr(e syntax.Expr, fail, succ label) (start, resume label, v vm.Operand) {
	if e == nil {
		return succ, fail, p.null()
	}
	if s, _, ok := syntax.Span(e); ok {
		p.setPos(s)
	}
	switch e := e.(type) {
	case *syntax.Ident:
		return succ, fail, p.ident(e.Name)
	case *syntax.Literal:
		return succ, fail, p.literal(e)
	case *syntax.Keyword:
		return p.keyword(e, fail, succ)
	case *syntax.Paren:
		if len(e.List.Elems) == 1 {
			return p.expr(e.List.Elems[0], fail, succ)
		}
		st, r, vs := p.seq(e.List.Elems, fail, succ)
		if len(vs) == 0 {
			return st, r, p.null()
		}
		return st, r, vs[len(vs)-1]
	case *syntax.Block:
		return p.block(e.List.Elems, fail, succ)
	case *syntax.Unary:
		return p.unary(e, fail, succ)
	case *syntax.Binary:
		return p.binary(e, fail, succ)
	case *syntax.ToBy:
		args := []syntax.Expr{e.X, e.Y}
		if e.Z != nil {
			args = append(args, e.Z)
		}
		return p.genOp(vm.Ops["to"], args, fail, succ)
	case *syntax.ListLit:
		return p.oper(vm.Ops["[...]"], e.List.Elems, fail, succ)
	case *syntax.Index:
		// x[i, j] is x[i][j]; x[] is x[&null], as in icont, whose empty
		// subscript list is one empty expression.
		subs := e.List.Elems
		if len(subs) == 0 {
			subs = []syntax.Expr{nil}
		}
		a := p.newLabel()
		st, r, vs := p.seq(append([]syntax.Expr{e.X}, subs...), fail, a)
		p.place(a)
		x := vs[0]
		for _, i := range vs[1:] {
			t := p.temp()
			next := p.newLabel()
			p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["[]"], Dst: t, Args: []vm.Operand{x, i}, L1: int32(next), L2: int32(r)})
			p.place(next)
			x = t
		}
		p.jump(succ)
		return st, r, x
	case *syntax.Section:
		if e.Op.Kind == token.COLON {
			return p.oper(vm.Ops["[:]"], []syntax.Expr{e.X, e.Lo, e.Hi}, fail, succ)
		}
		// x[i+:j] is x[i:i+j] and x[i-:j] is x[i:i-j], the arithmetic
		// being an operation of its own, as icont compiles it (so its
		// errors are the operator's: 102 for x[1+:"z"]).
		op := "+"
		if e.Op.Kind == token.MCOLON {
			op = "-"
		}
		a, b := p.newLabel(), p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Lo, e.Hi}, fail, a)
		p.place(a)
		hi := p.temp()
		p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops[op], Dst: hi, Args: []vm.Operand{vs[1], vs[2]}, L1: int32(b), L2: int32(r)})
		p.place(b)
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["[:]"], Dst: t, Args: []vm.Operand{vs[0], vs[1], hi}, L1: int32(succ), L2: int32(r)})
		return st, r, t
	case *syntax.Call:
		a := p.newLabel()
		st, r, vs := p.seq(append([]syntax.Expr{e.Fun}, e.Args.Elems...), fail, a)
		p.place(a)
		p.setLine(e.Lparen) // the call's line, for &trace
		return st, p.call(vs[0], vs[1:], r, succ), p.lastTemp(2)
	case *syntax.If:
		return p.ifExpr(e, fail, succ)
	case *syntax.Loop:
		return p.loop(e, fail, succ)
	case *syntax.Return:
		return p.ret(e, fail, succ)
	case *syntax.Break:
		return p.brk(e, fail)
	case *syntax.Next:
		if len(p.loops) == 0 {
			p.errorf(e, "invalid context for next")
			return fail, fail, p.null()
		}
		l := p.loops[len(p.loops)-1]
		return p.leave(l.next, l.scans), fail, p.null()
	case *syntax.Case:
		return p.caseExpr(e, fail, succ)
	case *syntax.Create:
		return p.create(e, fail, succ)
	case *syntax.PDCO:
		// p{e1, ..., en} is p([create e1, ..., create en]), as icont's
		// Pdco rules build it; an empty ei is create &null.
		creates := make([]syntax.Expr, len(e.Args.Elems))
		for i, x := range e.Args.Elems {
			creates[i] = &syntax.Create{Create: e.Lbrace, X: x}
		}
		list := &syntax.ListLit{Lbrack: e.Lbrace, List: syntax.List[syntax.Expr]{Elems: creates}, Rbrack: e.Rbrace}
		return p.expr(&syntax.Call{Fun: e.Fun, Lparen: e.Lbrace,
			Args: syntax.List[syntax.Expr]{Elems: []syntax.Expr{list}}, Rparen: e.Rbrace}, fail, succ)
	case *syntax.Field:
		f := p.c.fields[e.Name.Text]
		if f == nil {
			f = vm.FieldOp(e.Name.Text)
			p.c.fields[e.Name.Text] = f
		}
		return p.oper(f, []syntax.Expr{e.X}, fail, succ)
	default:
		p.errorf(e, "invalid expression")
	}
	return fail, fail, p.null()
}

// lastTemp returns the operand of the temp allocated n temps ago (for
// results of helpers that allocate a result temp and a handle temp).
func (p *pcomp) lastTemp(n int) vm.Operand {
	return vm.Operand{Kind: vm.TempOp, Index: int32(p.ntemps - n)}
}

// call emits a resumable call of fn with args at the current position:
// success goes to succ, failure to fail. It returns the resume label; the
// result is in the temp allocated two before the return (see lastTemp).
func (p *pcomp) call(fn vm.Operand, args []vm.Operand, fail, succ label) label {
	t, h := p.temp(), p.temp()
	p.emit(vm.Instr{Op: vm.OpCall, Dst: t, X: fn, H: h.Index, Args: args, L1: int32(succ), L2: int32(fail)})
	res := p.newLabel()
	p.place(res)
	p.emit(vm.Instr{Op: vm.OpResume, Dst: t, H: h.Index, L1: int32(succ), L2: int32(fail)})
	return res
}

// genOp compiles a generator operator applied to args.
func (p *pcomp) genOp(b *vm.Builtin, args []syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	a := p.newLabel()
	st, r, vs := p.seq(args, fail, a)
	p.place(a)
	fn := p.c.constant("op "+b.Name, vm.V(b))
	res := p.call(fn, vs, r, succ)
	return st, res, p.lastTemp(2)
}

// oper compiles an operator that produces at most one result.
func (p *pcomp) oper(b *vm.Builtin, args []syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	a := p.newLabel()
	st, r, vs := p.seq(args, fail, a)
	p.place(a)
	t := p.temp()
	p.emit(vm.Instr{Op: vm.OpOper, Fn: b, Dst: t, Args: vs, L1: int32(succ), L2: int32(r)})
	return st, r, t
}

// block compiles { e1; ...; en }: all but the last are bounded; the
// block's results are those of the last.
func (p *pcomp) block(es []syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	if len(es) == 0 {
		return succ, fail, p.null()
	}
	first := p.newLabel()
	p.place(first)
	for _, e := range es[:len(es)-1] {
		after := p.newLabel()
		p.stmt(e, after)
		p.place(after)
	}
	var r label
	var v vm.Operand
	p.enter(func() label {
		var st label
		st, r, v = p.expr(es[len(es)-1], fail, succ)
		return st
	})
	return first, r, v
}

// ref records that the procedure being compiled refers to a global.
func (p *pcomp) ref(name string) {
	r := p.c.refs[p.decl.Name.Text]
	if r == nil {
		r = map[string]bool{}
		p.c.refs[p.decl.Name.Text] = r
	}
	r[name] = true
}

func (p *pcomp) ident(t *syntax.Token) vm.Operand {
	s := p.info.Uses[t]
	if s == nil {
		p.errorf(&syntax.Ident{Name: t}, "unresolved identifier %s", t.Text)
		return p.null()
	}
	switch s.Kind {
	case sema.Param, sema.Local, sema.Implicit:
		if i, ok := p.locals[s]; ok {
			return vm.Operand{Kind: vm.LocalOp, Index: int32(i)}
		}
	case sema.Static:
		return vm.Operand{Kind: vm.GlobalOp, Index: int32(p.c.global(p.decl.Name.Text+" "+s.Name, vm.NullValue))}
	case sema.Global, sema.Procedure, sema.Record:
		p.ref(s.Name)
		return vm.Operand{Kind: vm.GlobalOp, Index: int32(p.c.global(s.Name, vm.NullValue))}
	case sema.Builtin:
		p.ref(s.Name)
		f := vm.Functions[s.Name]
		if f == nil {
			f = vm.Unsupported(s.Name)
		}
		return vm.Operand{Kind: vm.GlobalOp, Index: int32(p.c.global(s.Name, vm.V(f)))}
	}
	p.errorf(&syntax.Ident{Name: t}, "%s %s is not yet supported", s.Kind, s.Name)
	return p.null()
}

func (p *pcomp) literal(e *syntax.Literal) vm.Operand {
	t := e.Tok
	switch t.Kind {
	case token.STRINGLIT:
		s, u, err := lit.String(t.Text)
		if err != nil {
			p.errorf(e, "%v", err)
			return p.null()
		}
		unit := vm.TextUnit
		if u == lit.Bytes {
			unit = vm.BytesUnit
		}
		return p.c.constant(fmt.Sprintf("s%d%q", unit, s), vm.V(vm.NewString(s, unit)))
	case token.CSETLIT:
		segs, ranges, u, err := lit.Cset(t.Text)
		if err != nil {
			p.errorf(e, "%v", err)
			return p.null()
		}
		unit := vm.TextUnit
		if u == lit.Bytes {
			unit = vm.BytesUnit
		}
		rs := make([]vm.CsetRange, len(ranges))
		for i, r := range ranges {
			rs[i] = vm.CsetRange{Lo: r.Lo, Hi: r.Hi, LoneBytes: r.LoneBytes}
		}
		return p.c.constant(fmt.Sprintf("c%d%q%v", unit, segs, ranges), vm.V(vm.NewCsetLiteral(segs, rs, unit)))
	case token.INTLIT:
		n, err := lit.ParseNumber(t.Text)
		if err != nil {
			p.errorf(e, "%v", err)
		} else if n.Big != nil {
			return p.c.constant("I"+n.Big.String(), vm.NewInteger(n.Big))
		} else {
			return p.c.constant(fmt.Sprintf("i%d", n.Int), vm.V(vm.Int(n.Int)))
		}
	case token.REALLIT:
		n, err := lit.ParseNumber(t.Text)
		if err != nil {
			p.errorf(e, "%v", err)
		} else {
			return p.c.constant(fmt.Sprintf("r%v", n.Real), vm.V(vm.Real(n.Real)))
		}
	default:
		p.errorf(e, "invalid literal")
	}
	return p.null()
}

func (p *pcomp) keyword(e *syntax.Keyword, fail, succ label) (label, label, vm.Operand) {
	switch e.Name.Text {
	case "null":
		return succ, fail, p.null()
	case "fail":
		return fail, fail, p.null()
	case "pi":
		return succ, fail, p.c.constant("&pi", vm.V(vm.Real(math.Pi)))
	case "e":
		return succ, fail, p.c.constant("&e", vm.V(vm.Real(math.E)))
	case "phi":
		return succ, fail, p.c.constant("&phi", vm.V(vm.Real(math.Phi)))
	case "cset":
		return succ, fail, p.c.constant("&cset", vm.V(vm.CsetAll))
	case "ascii":
		return succ, fail, p.c.constant("&ascii", vm.V(vm.CsetASCII))
	case "digits":
		return succ, fail, p.c.constant("&digits", vm.V(vm.CsetDigits))
	case "lcase":
		return succ, fail, p.c.constant("&lcase", vm.V(vm.CsetLcase))
	case "ucase":
		return succ, fail, p.c.constant("&ucase", vm.V(vm.CsetUcase))
	case "letters":
		return succ, fail, p.c.constant("&letters", vm.V(vm.CsetLetters))
	case "uset":
		return succ, fail, p.c.constant("&uset", vm.V(vm.CsetUniv))
	case "current", "source", "main", "subject", "pos", "input", "output", "errout",
		"error", "errornumber", "errortext", "errorvalue", "line", "file", "random",
		"trace", "level", "clock", "date", "dateline", "time", "host", "version",
		"progname", "dump", "eventcode", "eventsource", "eventvalue",
		// The graphics keywords: gcon-vm has graphics; gcon's VM
		// does not, and its keywords fail, as in an Icon without them.
		"col", "row", "x", "y", "interval", "control", "shift", "meta", "window":
		s := p.newLabel()
		p.place(s)
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["&"+e.Name.Text], Dst: t, L1: int32(succ), L2: int32(fail)})
		return s, fail, t
	}
	switch e.Name.Text {
	case "features", "collections", "regions", "storage", "allocated":
		return p.genOp(vm.Ops["&"+e.Name.Text], nil, fail, succ)
	case "column":
		// &column fails in iconx.
		return fail, fail, p.null()
	}
	if n, ok := vm.MouseEvents[e.Name.Text]; ok {
		return succ, fail, p.c.constant(fmt.Sprintf("i%d", n), vm.V(n))
	}
	p.errorf(e, "&%s is not yet supported", e.Name.Text)
	return fail, fail, p.null()
}

// unaryOps maps prefix operators to their implementations.
var unaryOps = map[string]string{"-": "u-", "+": "u+", "*": "u*", "~": "u~"}

// compoundUnary are the tokens that, in prefix position, are sequences
// of unary operators, applied right to left as icont's unopb does:
// ++x is +(+x) and ~==x is ~(=(=x)).
var compoundUnary = map[string][]string{
	"++": {"+", "+"}, "--": {"-", "-"}, "**": {"*", "*"},
	"==": {"=", "="}, "===": {"=", "=", "="}, "~=": {"~", "="},
	"~==": {"~", "=", "="}, "~===": {"~", "=", "=", "="},
}

func (p *pcomp) unary(e *syntax.Unary, fail, succ label) (label, label, vm.Operand) {
	op := e.Op.Text
	if seq, ok := compoundUnary[op]; ok {
		x := e.X
		for i := len(seq) - 1; i >= 0; i-- {
			t := *e.Op
			t.Text = seq[i]
			x = &syntax.Unary{Op: &t, X: x}
		}
		return p.unary(x.(*syntax.Unary), fail, succ)
	}
	switch {
	case op == "|" || op == "||" || op == "|||":
		return p.repalt(e.X, fail, succ)
	case unaryOps[op] != "":
		return p.oper(vm.Ops[unaryOps[op]], []syntax.Expr{e.X}, fail, succ)
	case op == "!":
		return p.genOp(vm.Ops["u!"], []syntax.Expr{e.X}, fail, succ)
	case op == "=":
		return p.genOp(vm.Ops["u="], []syntax.Expr{e.X}, fail, succ)
	case op == "/" || op == `\`:
		a := p.newLabel()
		st, r, v := p.expr(e.X, fail, a)
		p.place(a)
		in := vm.Instr{Op: vm.OpIsNull, X: v, L1: int32(succ), L2: int32(r)}
		if op == `\` {
			in.L1, in.L2 = in.L2, in.L1
		}
		p.emit(in)
		return st, r, v
	case op == "not":
		a := p.newLabel()
		st, _, _ := p.expr(e.X, a, fail)
		p.place(a)
		p.jump(succ)
		return st, fail, p.null()
	case op == "@":
		return p.activate(nil, e.X, fail, succ)
	case op == "^":
		return p.oper(vm.Ops["u^"], []syntax.Expr{e.X}, fail, succ)
	case op == "?":
		return p.oper(vm.Ops["u?"], []syntax.Expr{e.X}, fail, succ)
	case op == ".":
		a := p.newLabel()
		st, r, v := p.expr(e.X, fail, a)
		p.place(a)
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpMove, Dst: t, X: v})
		p.jump(succ)
		return st, r, t
	}
	p.errorf(e, "unary %s is not yet supported", op)
	return fail, fail, p.null()
}

// binaryOps lists the infix operators implemented by vm.Ops.
var binaryOps = map[string]bool{
	"+": true, "-": true, "*": true, "/": true, "%": true, "^": true,
	"<": true, "<=": true, ">": true, ">=": true, "=": true, "~=": true,
	"==": true, "~==": true, "<<": true, "<<=": true, ">>": true, ">>=": true,
	"===": true, "~===": true, "||": true, "|||": true,
	"++": true, "--": true, "**": true,
}

func (p *pcomp) binary(e *syntax.Binary, fail, succ label) (label, label, vm.Operand) {
	op := e.Op.Text
	switch {
	case binaryOps[op]:
		return p.oper(vm.Ops[op], []syntax.Expr{e.X, e.Y}, fail, succ)
	case op == "&":
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, succ)
		return st, r, vs[1]
	case op == "|":
		return p.alt(e, fail, succ)
	case op == "@":
		return p.activate(e.X, e.Y, fail, succ)
	case op == `\`:
		return p.limit(e.X, e.Y, fail, succ)
	case op == ":=":
		a := p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: vs[1], L1: int32(succ), L2: int32(r)})
		return st, r, vs[0]
	case op == ":=:":
		a := p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpMove, Dst: t, X: vs[0]})
		b := p.newLabel()
		// Each assignment names the other operand (Args), which shifts if
		// both are substrings of one string (see vm's OpAssign).
		p.emit(vm.Instr{Op: vm.OpAssign, Fn: vm.AsgnOp(":=:"), Dst: vs[0], X: vs[1], Args: []vm.Operand{vs[1]}, L1: int32(b), L2: int32(r)})
		p.place(b)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[1], X: t, Args: []vm.Operand{vs[0]}, L1: int32(succ), L2: int32(r)})
		return st, r, vs[0]
	case op == "!":
		// p ! L calls p with the elements of list or record L.
		a := p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		t, h := p.temp(), p.temp()
		p.emit(vm.Instr{Op: vm.OpCall, Fn: vm.ApplyOp, Dst: t, X: vs[0], H: h.Index, Args: vs[1:], L1: int32(succ), L2: int32(r)})
		res := p.newLabel()
		p.place(res)
		p.emit(vm.Instr{Op: vm.OpResume, Dst: t, H: h.Index, L1: int32(succ), L2: int32(r)})
		return st, res, t
	case op == "@:=":
		// x @:= c is x := x @ c.
		a, b := p.newLabel(), p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpActivate, Dst: t, X: vs[1], Args: []vm.Operand{vs[0]}, L1: int32(b), L2: int32(r)})
		p.place(b)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: t, L1: int32(succ), L2: int32(r)})
		return st, r, vs[0]
	case op == "&:=":
		// x &:= y is x := (x & y), which is x := y with x evaluated first.
		a := p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: vs[1], L1: int32(succ), L2: int32(r)})
		return st, r, vs[0]
	case op == "<-":
		// x <- y assigns y to x; resumed, it restores x's old value and
		// fails (iconx's rasgn).
		a, rr := p.newLabel(), p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		old := p.temp()
		p.emit(vm.Instr{Op: vm.OpMove, Dst: old, X: vs[0]})
		p.emit(vm.Instr{Op: vm.OpAssign, Fn: vm.AsgnOp("<-"), Dst: vs[0], X: vs[1], L1: int32(succ), L2: int32(r)})
		p.place(rr)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: old, L1: int32(r), L2: int32(r)})
		return st, rr, vs[0]
	case op == "<->":
		// x <-> y exchanges x and y; resumed, it restores both and fails
		// (iconx's rswap).
		a, b, rr, c := p.newLabel(), p.newLabel(), p.newLabel(), p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		ox, oy := p.temp(), p.temp()
		p.emit(vm.Instr{Op: vm.OpMove, Dst: ox, X: vs[0]})
		p.emit(vm.Instr{Op: vm.OpMove, Dst: oy, X: vs[1]})
		p.emit(vm.Instr{Op: vm.OpAssign, Fn: vm.AsgnOp("<->"), Dst: vs[0], X: vs[1], Args: []vm.Operand{vs[1]}, L1: int32(b), L2: int32(r)})
		p.place(b)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[1], X: ox, Args: []vm.Operand{vs[0]}, L1: int32(succ), L2: int32(r)})
		// Resumed: restore x, then y. If an assignment fails (as &pos
		// := 3 does after &subject has been shortened), the operator
		// fails at once, leaving the other unrestored, as iconx's
		// rswap does.
		p.place(rr)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: ox, Args: []vm.Operand{vs[1]}, L1: int32(c), L2: int32(r)})
		p.place(c)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[1], X: oy, Args: []vm.Operand{vs[0]}, L1: int32(r), L2: int32(r)})
		return st, rr, vs[0]
	case op == "?":
		return p.scan(e.X, e.Y, false, fail, succ)
	case op == "?:=":
		return p.scan(e.X, e.Y, true, fail, succ)
	case strings.HasSuffix(op, ":=") && binaryOps[strings.TrimSuffix(op, ":=")]:
		// x op:= y is x := x op y, evaluating x once.
		a := p.newLabel()
		st, r, vs := p.seq([]syntax.Expr{e.X, e.Y}, fail, a)
		p.place(a)
		t := p.temp()
		b := p.newLabel()
		p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops[strings.TrimSuffix(op, ":=")], Dst: t, Args: vs, L1: int32(b), L2: int32(r)})
		p.place(b)
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vs[0], X: t, L1: int32(succ), L2: int32(r)})
		return st, r, vs[0]
	}
	p.errorf(e, "operator %s is not yet supported", op)
	return fail, fail, p.null()
}

// repalt compiles |e, repeated alternation: e's results, then e's
// results again, and so on, until e produces none.
func (p *pcomp) repalt(x syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	produced := p.temp() // non-null once e has produced a result this round
	entry, again, a, efail, reset := p.newLabel(), p.newLabel(), p.newLabel(), p.newLabel(), p.newLabel()
	null := p.null()
	p.place(entry)
	p.emit(vm.Instr{Op: vm.OpMove, Dst: produced, X: null})
	p.jump(again)
	st, r, v := p.expr(x, efail, a)
	p.bind(again, st)
	// e has no more results: stop if it produced none this round, else
	// start it again.
	p.place(efail)
	p.emit(vm.Instr{Op: vm.OpIsNull, X: produced, L1: int32(fail), L2: int32(reset)})
	p.place(reset)
	p.emit(vm.Instr{Op: vm.OpMove, Dst: produced, X: null})
	p.jump(again)
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpMove, Dst: produced, X: p.c.constant("i1", vm.V(vm.Int(1)))})
	t := p.temp()
	p.emit(vm.Instr{Op: vm.OpMoveVar, Dst: t, X: v})
	p.jump(succ)
	return entry, r, t
}

// alt compiles x | y. A gate records which alternative to resume.
func (p *pcomp) alt(e *syntax.Binary, fail, succ label) (label, label, vm.Operand) {
	t, gate := p.temp(), p.temp()
	ys, a, b := p.newLabel(), p.newLabel(), p.newLabel()
	st, r1, v1 := p.expr(e.X, ys, a)
	s2, r2, v2 := p.expr(e.Y, fail, b)
	p.bind(ys, s2)
	p.produce(a, gate, r1, t, v1, succ)
	p.produce(b, gate, r2, t, v2, succ)
	res := p.newLabel()
	p.place(res)
	p.emit(vm.Instr{Op: vm.OpIGoto, X: gate})
	return st, res, t
}

// produce places at l code that records resume in gate, puts the value
// v (as a variable, if it is one) in t, and goes to succ.
func (p *pcomp) produce(l label, gate vm.Operand, resume label, t, v vm.Operand, succ label) {
	p.place(l)
	p.emit(vm.Instr{Op: vm.OpSetLabel, Dst: gate, L1: int32(resume)})
	p.emit(vm.Instr{Op: vm.OpMoveVar, Dst: t, X: v})
	p.jump(succ)
}

func (p *pcomp) ifExpr(e *syntax.If, fail, succ label) (label, label, vm.Operand) {
	t, gate := p.temp(), p.temp()
	then, els := p.newLabel(), fail
	if e.Alt != nil {
		els = p.newLabel()
	}
	st, _, _ := p.expr(e.Cond, els, then)
	b := p.newLabel()
	s2, r2, v2 := p.expr(e.Body, fail, b)
	p.bind(then, s2)
	p.produce(b, gate, r2, t, v2, succ)
	if e.Alt != nil {
		c := p.newLabel()
		s3, r3, v3 := p.expr(e.Alt, fail, c)
		p.bind(els, s3)
		p.produce(c, gate, r3, t, v3, succ)
	}
	res := p.newLabel()
	p.place(res)
	p.emit(vm.Instr{Op: vm.OpIGoto, X: gate})
	return st, res, t
}

func (p *pcomp) loop(e *syntax.Loop, fail, succ label) (label, label, vm.Operand) {
	// The loop is the context of break and next in both its control
	// expression and its body: "while break" exits the while.
	l := &loop{next: p.newLabel(), fail: fail, succ: succ, result: p.temp(), gate: p.temp(),
		resume: p.newLabel(), scans: len(p.scans)}
	p.loops = append(p.loops, l)
	defer func() {
		p.loops = p.loops[:len(p.loops)-1]
		// Resuming the loop resumes the break expression that ended it.
		if l.broken {
			p.place(l.resume)
			p.emit(vm.Instr{Op: vm.OpIGoto, X: l.gate})
		} else {
			p.bind(l.resume, fail)
		}
	}()
	switch e.Keyword.Kind {
	case token.EVERY:
		body := p.newLabel()
		st, r, _ := p.expr(e.Cond, fail, body)
		p.bind(l.next, r)
		s2, _, _ := p.expr(e.Body, r, r)
		p.bind(body, s2)
		return st, l.resume, l.result
	case token.WHILE, token.UNTIL, token.REPEAT:
		top, cond := p.newLabel(), p.newLabel()
		p.place(top)
		p.jump(cond)
		p.bind(l.next, top)
		body := p.newLabel()
		switch e.Keyword.Kind {
		case token.WHILE:
			st, _, _ := p.expr(e.Cond, fail, body)
			p.bind(cond, st)
		case token.UNTIL:
			st, _, _ := p.expr(e.Cond, body, fail)
			p.bind(cond, st)
		default:
			p.bind(cond, body)
		}
		s2, _, _ := p.expr(e.Body, top, top)
		p.bind(body, s2)
		return top, l.resume, l.result
	}
	p.errorf(e, "invalid loop")
	return fail, fail, p.null()
}

// brk compiles break [e]: the loop produces e's results, e being
// evaluated in the loop's context; resuming the loop resumes e.
func (p *pcomp) brk(e *syntax.Break, fail label) (label, label, vm.Operand) {
	if len(p.loops) == 0 {
		p.errorf(e, "invalid context for break")
		return fail, fail, p.null()
	}
	l := p.loops[len(p.loops)-1]
	l.broken = true
	a := p.newLabel()
	savedLoops, savedScans := p.loops, p.scans
	p.loops, p.scans = p.loops[:len(p.loops)-1], p.scans[:l.scans]
	st, r, v := p.expr(e.X, l.fail, a)
	p.loops, p.scans = savedLoops, savedScans
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpSetLabel, Dst: l.gate, L1: int32(r)})
	p.emit(vm.Instr{Op: vm.OpMoveVar, Dst: l.result, X: v})
	p.jump(l.succ)
	return p.leave(st, l.scans), fail, p.null()
}

// leave returns a label that leaves the scans entered since depth (the
// innermost first), restoring their outer environments, and goes to
// target.
func (p *pcomp) leave(target label, depth int) label {
	if len(p.scans) == depth {
		return target
	}
	l := p.newLabel()
	p.place(l)
	for i := len(p.scans) - 1; i >= depth; i-- {
		p.emit(vm.Instr{Op: vm.OpScanSwap, X: p.scans[i]})
	}
	p.jump(target)
	return l
}

// failAt emits code that leaves the scans being compiled and fails the
// procedure, with the line of t, and returns its label.
func (p *pcomp) failAt(t *syntax.Token) label {
	save := p.line
	p.setLine(t)
	l := p.newLabel()
	p.place(l)
	for i := len(p.scans) - 1; i >= 0; i-- {
		p.emit(vm.Instr{Op: vm.OpScanSwap, X: p.scans[i]})
	}
	p.emit(vm.Instr{Op: vm.OpFail})
	p.line = save
	return l
}

// reenter emits code that re-enters the scans left by leave(_, 0), the
// outermost first.
func (p *pcomp) reenter() {
	for _, env := range p.scans {
		p.emit(vm.Instr{Op: vm.OpScanSwap, X: env})
	}
}

func (p *pcomp) ret(e *syntax.Return, fail, succ label) (label, label, vm.Operand) {
	if p.inCo > 0 {
		// As icont: a co-expression's body cannot leave its procedure.
		if e.Keyword.Kind == token.SUSPEND {
			p.errorf(e, "invalid context for suspend")
		} else {
			p.errorf(e, "invalid context for return or fail")
		}
		return fail, fail, p.null()
	}
	// Leaving the procedure from inside scans restores the caller's
	// scanning environment first, as iconx does; values are then
	// dereferenced, so "return &subject" returns the caller's subject.
	//
	// The instructions that leave carry the line of the keyword, which
	// is where &trace reports them, as iconx does.
	switch e.Keyword.Kind {
	case token.FAIL:
		return p.failAt(e.Keyword), fail, p.null()
	case token.RETURN:
		a := p.newLabel()
		st, _, v := p.expr(e.X, p.failAt(e.Keyword), a)
		p.place(a)
		for i := len(p.scans) - 1; i >= 0; i-- {
			p.emit(vm.Instr{Op: vm.OpScanSwap, X: p.scans[i]})
		}
		p.setLine(e.Keyword)
		p.emit(vm.Instr{Op: vm.OpReturn, X: v})
		return st, fail, p.null()
	case token.SUSPEND:
		a := p.newLabel()
		st, r, v := p.expr(e.X, fail, a)
		resume := r
		if e.Body != nil {
			s2, _, _ := p.expr(e.Body, r, r)
			resume = s2
		}
		p.place(a)
		for i := len(p.scans) - 1; i >= 0; i-- {
			p.emit(vm.Instr{Op: vm.OpScanSwap, X: p.scans[i]})
		}
		back := p.newLabel()
		p.setLine(e.Keyword)
		p.emit(vm.Instr{Op: vm.OpSuspend, X: v, L1: int32(back)})
		p.place(back)
		p.reenter()
		p.jump(resume)
		return st, fail, p.null()
	}
	p.errorf(e, "invalid return")
	return fail, fail, p.null()
}

// caseExpr compiles case x of { ... }: x's first value is compared (===)
// with each clause's labels in turn, the labels being generators; the
// default clause is tried last.
func (p *pcomp) caseExpr(e *syntax.Case, fail, succ label) (label, label, vm.Operand) {
	res, gate, x := p.temp(), p.temp(), p.temp()
	a := p.newLabel()
	st, _, vx := p.expr(e.X, fail, a)
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpMove, Dst: x, X: vx})
	next := p.newLabel()
	p.jump(next)
	var dflt *syntax.CaseClause
	body := func(cl *syntax.CaseClause) label {
		b := p.newLabel()
		s, r, v := p.expr(cl.Body, fail, b)
		p.produce(b, gate, r, res, v, succ)
		return s
	}
	for _, cl := range e.Clauses.Elems {
		if cl == nil {
			continue
		}
		if cl.Default != nil {
			dflt = cl
			continue
		}
		after, m := p.newLabel(), p.newLabel()
		s, r, v := p.expr(cl.Label, after, m)
		p.bind(next, s)
		p.place(m)
		bs := p.newLabel()
		p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["==="], Dst: p.temp(), Args: []vm.Operand{x, v}, L1: int32(bs), L2: int32(r)})
		p.bind(bs, body(cl))
		next = after
	}
	if dflt != nil {
		p.bind(next, body(dflt))
	} else {
		p.bind(next, fail)
	}
	r := p.newLabel()
	p.place(r)
	p.emit(vm.Instr{Op: vm.OpIGoto, X: gate})
	return st, r, res
}

// create compiles create e. The body is a separate region of the
// procedure's code, run in a frame of its own with copies of the
// creator's locals: each result is produced by OpCoRet, and exhaustion
// is OpCoFail.
func (p *pcomp) create(e *syntax.Create, fail, succ label) (label, label, vm.Operand) {
	saved := p.loops
	p.loops = nil // break and next cannot leave a co-expression
	savedScans := p.scans
	p.scans = nil
	p.inCo++
	a, done := p.newLabel(), p.newLabel()
	body, r, v := p.expr(e.X, done, a)
	p.inCo--
	p.loops, p.scans = saved, savedScans
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpCoRet, X: v, L1: int32(r)})
	p.place(done)
	p.emit(vm.Instr{Op: vm.OpCoFail})
	s := p.newLabel()
	p.place(s)
	t := p.temp()
	p.emit(vm.Instr{Op: vm.OpCreate, Dst: t, L1: int32(body)})
	p.jump(succ)
	return s, fail, t
}

// activate compiles x @ c, or @c if x is nil (transmitting &null).
func (p *pcomp) activate(x, c syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	es := []syntax.Expr{c}
	if x != nil {
		es = []syntax.Expr{x, c}
	}
	a := p.newLabel()
	st, r, vs := p.seq(es, fail, a)
	p.place(a)
	t := p.temp()
	in := vm.Instr{Op: vm.OpActivate, Dst: t, X: vs[len(vs)-1], L1: int32(succ), L2: int32(r)}
	if x != nil {
		in.Args = []vm.Operand{vs[0]}
	}
	p.emit(in)
	return st, r, t
}

// scan compiles x ? y (or x ?:= y when assign is set, which assigns the
// result to x), following Jcon: entering y saves the outer scanning
// environment; when y produces a value, the environments are exchanged,
// so the outer one is current; resuming y exchanges them back; when y
// fails, the outer one is restored.
func (p *pcomp) scan(x, y syntax.Expr, assign bool, fail, succ label) (label, label, vm.Operand) {
	a := p.newLabel()
	st, r1, vx := p.expr(x, fail, a)
	env := p.temp()
	b, d, res := p.newLabel(), p.newLabel(), p.newLabel()
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpScanEnter, Dst: env, X: vx, L2: int32(r1)})
	var r2 label
	var vy vm.Operand
	p.scans = append(p.scans, env)
	p.enter(func() label {
		var s2 label
		s2, r2, vy = p.expr(y, d, b)
		return s2
	})
	p.scans = p.scans[:len(p.scans)-1]
	// y succeeded: back to the outer environment.
	p.place(b)
	p.emit(vm.Instr{Op: vm.OpScanSwap, X: env})
	v := vy
	if assign {
		t := p.temp()
		p.emit(vm.Instr{Op: vm.OpMove, Dst: t, X: vy})
		p.emit(vm.Instr{Op: vm.OpAssign, Dst: vx, X: t, L1: int32(succ), L2: int32(res)})
		v = vx
	} else {
		p.jump(succ)
	}
	// Resumed (or the assignment failed): back into y's environment.
	p.place(res)
	p.emit(vm.Instr{Op: vm.OpScanSwap, X: env})
	p.jump(r2)
	// y failed: restore the outer environment.
	p.place(d)
	p.emit(vm.Instr{Op: vm.OpScanSwap, X: env})
	p.jump(r1)
	return st, res, v
}

// limit compiles e \ n: n is evaluated first, then e produces at most n
// results; when e's results run out, n is resumed and e starts again (so
// (1 to 10) \ (1 | 2) produces 1, 1, 2).
func (p *pcomp) limit(e, n syntax.Expr, fail, succ label) (label, label, vm.Operand) {
	a := p.newLabel()
	st, rn, vn := p.expr(n, fail, a)
	lim, cnt, junk := p.temp(), p.temp(), p.temp()
	b, c, res := p.newLabel(), p.newLabel(), p.newLabel()
	p.place(a)
	p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["limit"], Dst: lim, Args: []vm.Operand{vn}, L1: int32(b), L2: int32(rn)})
	p.place(b)
	p.emit(vm.Instr{Op: vm.OpMove, Dst: cnt, X: p.c.constant("i0", vm.V(vm.Int(0)))})
	var re label
	var ve vm.Operand
	p.enter(func() label {
		var se label
		se, re, ve = p.expr(e, rn, c)
		return se
	})
	// e produced a result: count it.
	p.place(c)
	p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["+"], Dst: cnt, Args: []vm.Operand{cnt, p.c.constant("i1", vm.V(vm.Int(1)))}, L1: int32(succ), L2: int32(succ)})
	// Resumed: resume e if the limit allows, else resume n.
	p.place(res)
	p.emit(vm.Instr{Op: vm.OpOper, Fn: vm.Ops["<"], Dst: junk, Args: []vm.Operand{cnt, lim}, L1: int32(re), L2: int32(rn)})
	return st, res, ve
}
