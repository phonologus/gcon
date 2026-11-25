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
//
// The numbers are part of the built program's format (encode.go): an
// opcode is never renumbered or reused, and adding one, or changing what
// an instruction does with its fields, changes FormatVersion.
const (
	OpGoto      Op = 0  // goto L1
	OpIGoto     Op = 1  // goto the code index held in temp X
	OpSetLabel  Op = 2  // Dst := L1 (for OpIGoto)
	OpMove      Op = 3  // Dst := Deref(X)
	OpMoveVar   Op = 4  // Dst := X as a variable (locals and globals become references)
	OpAssign    Op = 5  // variable Dst := Deref(X); success goto L1, failure goto L2
	OpOper      Op = 6  // Dst := Fn(Args...); success goto L1, failure goto L2
	OpCall      Op = 7  // Dst := X(Args...), resumable through temp H; success L1, failure L2
	OpResume    Op = 8  // Dst := next result of temp H; success L1, failure L2
	OpReturn    Op = 9  // return Deref(X)
	OpFail      Op = 10 // fail
	OpSuspend   Op = 11 // suspend Deref(X); when resumed, goto L1
	OpOnce      Op = 12 // if global X is set goto L1, else set it and continue (initial clauses)
	OpIsNull    Op = 13 // if Deref(X) is &null goto L1 else goto L2
	OpCreate    Op = 14 // Dst := a new co-expression whose body starts at L1
	OpActivate  Op = 15 // Dst := result of activating co-expression X, transmitting Args[0]; success L1, failure L2
	OpCoRet     Op = 16 // the running co-expression produces Deref(X); when resumed, goto L1
	OpCoFail    Op = 17 // the running co-expression is exhausted
	OpScanEnter Op = 18 // Dst := the scanning environment; start scanning Deref(X); goto L2 if a converted error fails it
	OpScanSwap  Op = 19 // exchange the scanning environment with the one in temp X

	numOps = 20
)

var opNames = [numOps]string{"goto", "igoto", "setlabel", "move", "movevar", "assign",
	"oper", "call", "resume", "return", "fail", "suspend", "once", "isnull",
	"create", "activate", "coret", "cofail", "scanenter", "scanswap"}

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
	File   int32 // 0: the procedure's file; k: its Files[k-1]
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
	case OpOper, OpCall, OpResume, OpIsNull, OpActivate, OpAssign:
		s += fmt.Sprintf(" ->%d/%d", in.L1, in.L2)
	case OpScanEnter:
		s += fmt.Sprintf(" fail->%d", in.L2)
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
	// Invocable says which globals string invocation can find, by
	// index; nil means all of them ("invocable all").
	Invocable []bool
}
