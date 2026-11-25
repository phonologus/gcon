package vm

import "testing"

// TestOpNames checks that every opcode has a name (opNames has numOps
// entries, so a missing one is empty).
func TestOpNames(t *testing.T) {
	for op := Op(0); op < numOps; op++ {
		if opNames[op] == "" {
			t.Errorf("opcode %d has no name", op)
		}
	}
}
