package vm

import "math/big"

// ?x and &random, as iconx's random operator (src/runtime/oref.r). The
// generator is iconx's linear congruential one, so a program that sets
// &random gets iconx's sequence of results:
//
//	&random := (1103515245 * &random + 453816694) mod 2^31
//
// scaled by 1/(2^31 - 1) to a real in [0, 1). &random starts at 0.

// randVal advances &random and returns iconx's RandVal.
func (vm *VM) randVal() float64 {
	vm.random = Int((1103515245*uint64(vm.random) + 453816694) & 0x7fffffff)
	return 4.65661286e-10 * float64(vm.random)
}

// randIndex returns a random index in 0..n-1 (n > 0), as iconx's
// (word)(RandVal * n).
func (vm *VM) randIndex(n int) int {
	return int(vm.randVal() * float64(n))
}

// randomChar returns a random character of s, or fails if s is empty.
// A character is one in s's unit, as !s produces.
func (vm *VM) randomChar(s *String) (Value, bool) {
	n := s.Runes()
	if n == 0 {
		return nil, false
	}
	k := vm.randIndex(n)
	if n == s.Len() {
		return s.Slice(k+1, k+2), true
	}
	i := 1
	for ; k > 0; k-- {
		i += s.width(i - 1)
	}
	return s.Slice(i, i+s.width(i-1)), true
}

// random is ?x. Elements of lists, records and tables are produced as
// variables, so ?L := x assigns to a random element. Tables and sets
// choose among their entries in insertion order, where iconx uses its
// hash order. A character of a string is a value, not a variable (as
// for s[i]; see "Deliberate simplifications").
func random(vm *VM, a []Value) (Value, bool) {
	switch x := a[0].(type) {
	case *String:
		return vm.randomChar(x)
	case *Cset:
		return vm.randomChar(x.String())
	case *List:
		if len(x.Elems) == 0 {
			return nil, false
		}
		return ListElem{x, vm.randIndex(len(x.Elems))}, true
	case *Record:
		if len(x.Fields) == 0 {
			return nil, false
		}
		return FieldVar{x, vm.randIndex(len(x.Fields))}, true
	case *Table:
		if k, ok := vm.randomEntry(&x.m); ok {
			return TableElem{x, k}, true
		}
		return nil, false
	case *Set:
		return vm.randomEntry(&x.m)
	}
	n, ok := CvInteger(a[0])
	if !ok {
		Runerr(113, a[0])
	}
	if b, isBig := n.(*Big); isBig {
		return vm.bigRand(b.v), true
	}
	v := n.(Int)
	switch {
	case v < 0:
		Runerr(205, a[0])
	case v == 0:
		return Real(vm.randVal()), true
	}
	return Int(int64(vm.randVal()*float64(v))) + 1, true
}

// randomEntry returns the key of a random live entry of m.
func (vm *VM) randomEntry(m *omap) (Value, bool) {
	n := m.len()
	if n == 0 {
		return nil, false
	}
	k := vm.randIndex(n)
	for _, key := range m.keys {
		if key == nil {
			continue // deleted
		}
		if k == 0 {
			return key, true
		}
		k--
	}
	panic("vm: random entry out of range")
}

// bigRand returns ?b for a large integer, as iconx's bigrand: a random
// number of one more base 2^32 digit than |b|, made from successive
// random values starting with the least significant digit, reduced
// modulo |b|, plus 1.
func (vm *VM) bigRand(b *big.Int) Value {
	var a big.Int
	a.Abs(b)
	alen := (a.BitLen() + 31) / 32
	digits := make([]uint32, alen+1) // most significant first
	for i := alen; i >= 0; i-- {
		digits[i] = uint32(vm.randVal() * (1 << 32))
	}
	d := new(big.Int)
	for _, g := range digits {
		d.Lsh(d, 32)
		d.Or(d, big.NewInt(int64(g)))
	}
	d.Mod(d, &a)
	return mkInteger(d.Add(d, big.NewInt(1)))
}

// randomVar is the variable &random.
type randomVar struct{ vm *VM }

func (k randomVar) Get() Value { return k.vm.random }
func (k randomVar) Set(v Value) bool {
	k.vm.random = MustInt(v)
	return true
}

func init() {
	Ops["u?"] = &Builtin{Name: "?", Fn: random}
	Ops["&random"] = &Builtin{Name: "&random", Fn: func(vm *VM, a []Value) (Value, bool) {
		return randomVar{vm}, true
	}}
}
