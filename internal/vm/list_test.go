package vm

import (
	"math/rand/v2"
	"testing"
)

// TestListDeque checks push, put, get and pull on a list against a plain
// slice, over random sequences, including puts that move the elements
// out of the array pushFront made (so that its room may not be reused).
func TestListDeque(t *testing.T) {
	vm := testVM()
	call := func(name string, args ...Value) (Value, bool) {
		return Functions[name].Fn(vm, args)
	}
	r := rand.New(rand.NewPCG(1, 2))
	for trial := 0; trial < 200; trial++ {
		l := vm.NewList(nil)
		lv := ListVal(l)
		var model []Int
		next := Int(0)
		for step := 0; step < 300; step++ {
			switch r.IntN(4) {
			case 0:
				next++
				call("push", lv, IntVal(next))
				model = append([]Int{next}, model...)
			case 1:
				n := 1 + r.IntN(3)
				args := []Value{lv}
				for range n {
					next++
					args = append(args, IntVal(next))
					model = append(model, next)
				}
				call("put", args...)
			case 2:
				v, ok := call("get", lv)
				if ok != (len(model) > 0) || ok && v.Int() != model[0] {
					t.Fatalf("trial %d step %d: get gave %v %v, model %v", trial, step, v, ok, model)
				}
				if ok {
					model = model[1:]
				}
			case 3:
				v, ok := call("pull", lv)
				if ok != (len(model) > 0) || ok && v.Int() != model[len(model)-1] {
					t.Fatalf("trial %d step %d: pull gave %v %v", trial, step, v, ok)
				}
				if ok {
					model = model[:len(model)-1]
				}
			}
			if len(l.Elems) != len(model) {
				t.Fatalf("trial %d step %d: length %d, want %d", trial, step, len(l.Elems), len(model))
			}
			for i, m := range model {
				if l.Elems[i].Int() != m {
					t.Fatalf("trial %d step %d: element %d is %d, want %d", trial, step, i, l.Elems[i].Int(), m)
				}
			}
		}
	}
}
