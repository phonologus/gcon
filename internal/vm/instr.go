package vm

import "fmt"

// OperandKind says where an operand lives.
type OperandKind uint8

const (
	NoOperand OperandKind = iota
	TempOp                // frame temporary
	LocalOp               // parameter or local variable
	GlobalOp              // global variable (globals, procedures, statics)
	ConstOp               // program constant
)

// Operand names a value. Local and global operands are variables, read
// when the instruction using them executes: Icon dereferences operands
// late, so write(x, x := 2) writes "22".
type Operand struct {
	Kind  OperandKind
	Index int32
}

func (o Operand) String() string {
	switch o.Kind {
	case TempOp:
		return fmt.Sprintf("t%d", o.Index)
	case LocalOp:
		return fmt.Sprintf("l%d", o.Index)
	case GlobalOp:
		return fmt.Sprintf("g%d", o.Index)
	case ConstOp:
		return fmt.Sprintf("k%d", o.Index)
	}
	return "-"
}

// Op is an instruction opcode.
type Op uint8

// Instructions. Unless stated otherwise an instruction continues with
// the next one. L1 and L2 are code indices.
const (
	OpGoto     Op = iota // goto L1
	OpIGoto              // goto the code index held in temp X
	OpSetLabel           // Dst := L1 (for OpIGoto)
	OpMove               // Dst := Deref(X)
	OpMoveVar            // Dst := X as a variable (locals and globals become references)
	OpAssign             // variable Dst := Deref(X)
	OpOper               // Dst := Fn(Args...); success goto L1, failure goto L2
	OpCall               // Dst := X(Args...), resumable through temp H; success L1, failure L2
	OpResume             // Dst := next result of temp H; success L1, failure L2
	OpReturn             // return Deref(X)
	OpFail               // fail
	OpSuspend            // suspend Deref(X); when resumed, goto L1
	OpOnce               // if global X is set goto L1, else set it and continue (initial clauses)
	OpIsNull             // if Deref(X) is &null goto L1 else goto L2
	OpCreate             // Dst := a new co-expression whose body starts at L1
	OpActivate           // Dst := result of activating co-expression X, transmitting Args[0]; success L1, failure L2
	OpCoRet              // the running co-expression produces Deref(X); when resumed, goto L1
	OpCoFail             // the running co-expression is exhausted
)

var opNames = [...]string{"goto", "igoto", "setlabel", "move", "movevar", "assign",
	"oper", "call", "resume", "return", "fail", "suspend", "once", "isnull",
	"create", "activate", "coret", "cofail"}

func (op Op) String() string { return opNames[op] }

// Instr is one instruction.
type Instr struct {
	Op     Op
	Dst    Operand
	X      Operand
	H      int32 // handle temp for OpCall and OpResume
	Args   []Operand
	Fn     *Builtin // for OpOper
	L1, L2 int32
	Line   int32
}

func (in *Instr) String() string {
	s := in.Op.String()
	if in.Fn != nil {
		s += " " + in.Fn.Name
	}
	if in.Dst.Kind != NoOperand {
		s += " " + in.Dst.String() + " :="
	}
	if in.X.Kind != NoOperand {
		s += " " + in.X.String()
	}
	for _, a := range in.Args {
		s += " " + a.String()
	}
	switch in.Op {
	case OpGoto, OpSetLabel, OpOnce, OpSuspend, OpCreate, OpCoRet:
		s += fmt.Sprintf(" ->%d", in.L1)
	case OpOper, OpCall, OpResume, OpIsNull, OpActivate:
		s += fmt.Sprintf(" ->%d/%d", in.L1, in.L2)
	}
	if in.Op == OpCall || in.Op == OpResume {
		s += fmt.Sprintf(" h=t%d", in.H)
	}
	return s
}

// Program is a compiled program.
type Program struct {
	Consts      []Value
	Globals     []Value // initial values of the globals
	GlobalNames []string
	Main        int // global index of main, or -1
}
