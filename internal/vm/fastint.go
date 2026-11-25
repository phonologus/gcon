package vm

// Integers on the VM's fast paths.
//
// A Value is an interface, so storing an Int in one allocates unless Go
// has the value preboxed (it has 0 to 255). Small integers are common,
// so mkInt hands out boxes made once, at start-up, for a wider range;
// they are made together so that they share cache lines (boxes made
// lazily, scattered over the heap, measured slower than new ones for a
// counter running through them). And the VM computes integer arithmetic
// and comparison itself (fastInt), without calling the operator's
// function, which converts its operands in general.

const (
	minCachedInt = -256
	maxCachedInt = 4095
)

var intBoxes = func() (b [maxCachedInt - minCachedInt + 1]Value) {
	for i := range b {
		b[i] = Int(i + minCachedInt)
	}
	return b
}()

// mkInt returns i as a Value, reusing a box for a small integer.
func mkInt(i Int) Value {
	if i < minCachedInt || i > maxCachedInt {
		return i
	}
	return intBoxes[i-minCachedInt]
}

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
)

func init() {
	for k, op := range map[string]intOp{"+": intAdd, "-": intSub, "*": intMul,
		"<": intLT, "<=": intLE, ">": intGT, ">=": intGE, "=": intEQ, "~=": intNE} {
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
		return nil, false, false
	}
	return mkInt(r), true, true
}
