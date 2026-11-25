package vm

import "math"

// Integers on the VM's fast paths.
//
// The VM computes integer arithmetic and comparison itself (fastInt),
// without calling the operator's function, which converts its operands
// in general.

// mkInt returns i as a Value. (While a Value was an interface, it handed
// out preboxed small integers; now an Int is held in the Value.)
func mkInt(i Int) Value { return IntVal(i) }

// intOp is a binary operator the VM computes inline for Ints.
type intOp uint8

const (
	intNone intOp = iota
	intAdd
	intSub
	intMul
	intLT
	intLE
	intGT
	intGE
	intEQ
	intNE
	// Not only for Ints (fastOp): unary *, + and -, and === and ~===.
	opSize
	opPlus
	opNeg
	opIdent
	opNotIdent
)

func init() {
	for k, op := range map[string]intOp{"+": intAdd, "-": intSub, "*": intMul,
		"<": intLT, "<=": intLE, ">": intGT, ">=": intGE, "=": intEQ, "~=": intNE,
		"u*": opSize, "u+": opPlus, "u-": opNeg, "===": opIdent, "~===": opNotIdent} {
		Ops[k].intOp = op
	}
}

// fastInt computes op on Ints x and y, whose boxed values are xv and yv:
// the result and whether the operation succeeds. done is false if the
// result does not fit in an Int, for the operator's function to compute.
// A comparison produces its right operand, as numCompare does.
func fastInt(op intOp, x, y Int, yv Value) (v Value, ok, done bool) {
	var r Int
	switch op {
	case intAdd:
		r, done = addInt(x, y)
	case intSub:
		r, done = subInt(x, y)
	case intMul:
		r, done = mulInt(x, y)
	case intLT:
		return yv, x < y, true
	case intLE:
		return yv, x <= y, true
	case intGT:
		return yv, x > y, true
	case intGE:
		return yv, x >= y, true
	case intEQ:
		return yv, x == y, true
	case intNE:
		return yv, x != y, true
	}
	if !done {
		return Value{}, false, false
	}
	return mkInt(r), true, true
}

// fastOp computes a unary operator or an identity test for the common
// cases, reporting done false for the operator's function to handle the
// rest (a string to convert to a number, for instance).
func fastOp(op intOp, x, y Value) (v Value, ok, done bool) {
	switch op {
	case opSize:
		switch x.k {
		case KString:
			return IntVal(Int(x.n & strLenMask)), true, true
		case KList:
			return IntVal(Int(len(x.List().Elems))), true, true
		case KTable:
			return IntVal(Int(x.Table().m.len())), true, true
		case KSet:
			return IntVal(Int(x.Set().m.len())), true, true
		case KRecord:
			return IntVal(Int(len(x.Record().Fields))), true, true
		case KCset:
			return IntVal(Int(x.Cset().Size())), true, true
		}
	case opPlus:
		if x.k == KInt || x.k == KReal {
			return x, true, true
		}
	case opNeg:
		if x.k == KInt && x.Int() != math.MinInt64 {
			return IntVal(-x.Int()), true, true
		}
		if x.k == KReal {
			return RealVal(-x.Real()), true, true
		}
	case opIdent:
		return y, Identical(x, y), true
	case opNotIdent:
		return y, !Identical(x, y), true
	}
	return Value{}, false, false
}
