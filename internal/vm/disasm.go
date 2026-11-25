package vm

import (
	"bufio"
	"fmt"
	"io"
	"sort"
	"strings"
)

// Disasm writes a listing of p: its build information, constants,
// globals and procedures, each instruction with its index and line.
func Disasm(w io.Writer, p *Program, info Info) error {
	b := bufio.NewWriter(w)
	fmt.Fprintf(b, "format %d\n", FormatVersion)
	keys := make([]string, 0, len(info))
	for k := range info {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(b, "info %s: %s\n", k, info[k])
	}

	fmt.Fprintf(b, "\nconstants\n")
	for i, v := range p.Consts {
		fmt.Fprintf(b, "  k%-4d %s\n", i, constImage(v))
	}

	fmt.Fprintf(b, "\nglobals\n")
	for i, v := range p.Globals {
		fmt.Fprintf(b, "  g%-4d %-16s %s\n", i, p.GlobalNames[i], constImage(v))
	}
	if p.Main >= 0 {
		fmt.Fprintf(b, "main g%d\n", p.Main)
	} else {
		fmt.Fprintf(b, "no main\n")
	}
	if p.Invocable == nil {
		fmt.Fprintf(b, "invocable all\n")
	} else {
		fmt.Fprintf(b, "invocable")
		for i, ok := range p.Invocable {
			if ok {
				fmt.Fprintf(b, " g%d", i)
			}
		}
		fmt.Fprintf(b, "\n")
	}

	seen := map[*Proc]bool{}
	for _, v := range p.Globals {
		if pr, ok := v.(*Proc); ok && !seen[pr] {
			seen[pr] = true
			disasmProc(b, pr)
		}
	}
	return b.Flush()
}

func disasmProc(b *bufio.Writer, p *Proc) {
	fmt.Fprintf(b, "\nprocedure %s (%s)\n", p.Name, p.File)
	params := fmt.Sprint(p.NParams)
	if p.Variadic {
		params += " variadic"
	}
	fmt.Fprintf(b, "  params %s; locals %d", params, p.NLocals)
	if len(p.LocalNames) > 0 {
		fmt.Fprintf(b, " (%s)", strings.Join(p.LocalNames, " "))
	}
	fmt.Fprintf(b, "; temps %d", p.NTemps)
	if len(p.Statics) > 0 {
		fmt.Fprintf(b, "; statics")
		for _, s := range p.Statics {
			fmt.Fprintf(b, " g%d", s)
		}
	}
	fmt.Fprintf(b, "\n")
	for i, f := range p.Files {
		fmt.Fprintf(b, "  file %d: %s\n", i+1, f)
	}
	for pc := range p.Code {
		fmt.Fprintf(b, "  %5d %5d  %s\n", pc, p.Code[pc].Line, InstrText(&p.Code[pc]))
	}
}

// InstrText returns a complete listing of an instruction: unlike
// Instr.String, it names a builtin by its reference (op u- rather than
// -) and shows every nonzero field.
func InstrText(in *Instr) string {
	var b strings.Builder
	b.WriteString(in.Op.String())
	if in.Fn != nil {
		if r, ok := RefOf(in.Fn); ok {
			fmt.Fprintf(&b, " [%s]", r)
		} else {
			fmt.Fprintf(&b, " [? %s]", in.Fn.Name)
		}
	}
	if in.Dst.Kind != NoOperand {
		b.WriteString(" " + in.Dst.String() + " :=")
	}
	if in.X.Kind != NoOperand {
		b.WriteString(" " + in.X.String())
	}
	for _, a := range in.Args {
		b.WriteString(" " + a.String())
	}
	switch in.Op {
	case OpGoto, OpSetLabel, OpOnce, OpSuspend, OpCreate, OpCoRet:
		fmt.Fprintf(&b, " ->%d", in.L1)
		if in.L2 != 0 {
			fmt.Fprintf(&b, " L2=%d", in.L2)
		}
	case OpOper, OpCall, OpResume, OpIsNull, OpActivate, OpAssign:
		fmt.Fprintf(&b, " ->%d/%d", in.L1, in.L2)
	case OpScanEnter:
		fmt.Fprintf(&b, " fail->%d", in.L2)
		if in.L1 != 0 {
			fmt.Fprintf(&b, " L1=%d", in.L1)
		}
	default:
		if in.L1 != 0 {
			fmt.Fprintf(&b, " L1=%d", in.L1)
		}
		if in.L2 != 0 {
			fmt.Fprintf(&b, " L2=%d", in.L2)
		}
	}
	if in.H != 0 || in.Op == OpCall || in.Op == OpResume {
		fmt.Fprintf(&b, " h=t%d", in.H)
	}
	if in.File != 0 {
		fmt.Fprintf(&b, " file=%d", in.File)
	}
	return b.String()
}

// constImage returns the listing of a constant or a global's initial
// value.
func constImage(v Value) string {
	switch x := v.(type) {
	case Null:
		return "&null"
	case Int:
		return itoa(x)
	case *Big:
		return bigImage(x.v)
	case Real:
		return rtos(float64(x))
	case *String:
		s := quote(x, '"')
		if x.unit == BytesUnit {
			s = "bytes " + s
		}
		return s
	case *Cset:
		return imageCset(x)
	case *Proc:
		return "procedure " + x.Name
	case *RecordType:
		return "record " + x.Name + "(" + strings.Join(x.Fields, ", ") + ")"
	case *Builtin:
		if r, ok := RefOf(x); ok {
			return r.String()
		}
		return "? " + x.Name
	}
	return fmt.Sprintf("? %T", v)
}
