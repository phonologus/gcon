package vm

import "testing"

// TestFuncGenInValue checks that a funcGen survives being held in a
// Value by its closure pointer (gen.go), with its captured state.
func TestFuncGenInValue(t *testing.T) {
	n := 0
	limit := 3
	g := funcGen(func() (Value, bool) {
		if n >= limit {
			return Value{}, false
		}
		n++
		return IntVal(Int(n * 10)), true
	})
	v := genVal(g)
	if v.k != KGen || v.n != genFunc {
		t.Fatalf("not held as a funcGen: %+v", v)
	}
	var got []Int
	for {
		r, ok := genNext(v, nil)
		if !ok {
			break
		}
		got = append(got, r.Int())
	}
	if len(got) != 3 || got[0] != 10 || got[2] != 30 {
		t.Errorf("got %v", got)
	}
	if _, ok := v.gen().(funcGen); !ok {
		t.Error("gen() is not a funcGen")
	}
	if r, ok := genNext(genVal(failGen{}), nil); ok || !r.IsNone() {
		t.Error("failGen produced a result")
	}
}
