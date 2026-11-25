package vm

import (
	"math"
	"math/big"
	"strconv"
)

// Large integers, as iconx's (src/runtime/rlrgint.r). An Icon integer is
// an Int when it fits in 64 bits and a *Big otherwise: arithmetic on Ints
// takes the fast path and moves to math/big only when a result
// overflows. Every result is normalised, so a Big never holds a value
// that fits in an Int; === and table keys can therefore compare Bigs by
// value, and the two types never meet as equal values.

// Big is an integer outside Int's range. Its value is never modified.
type Big struct{ v *big.Int }

// mkInteger returns b as an Icon integer: an Int if it fits, else a Big.
// b must not be modified afterwards.
func mkInteger(b *big.Int) Value {
	if b.IsInt64() {
		return Int(b.Int64())
	}
	return &Big{b}
}

// isInteger reports whether v is an integer, small or large.
func isInteger(v Value) bool {
	switch v.(type) {
	case Int, *Big:
		return true
	}
	return false
}

// bigOf returns integer v (an Int or *Big) as a big.Int, which the
// caller must not modify.
func bigOf(v Value) *big.Int {
	if b, ok := v.(*Big); ok {
		return b.v
	}
	return big.NewInt(int64(v.(Int)))
}

// bigToReal converts b to a real as iconx's bigtoreal does, digit by
// digit in base 2^32 from the most significant, so that the rounding is
// the same. It fails if the result is infinite.
func bigToReal(b *big.Int) (float64, bool) {
	var a big.Int
	a.Abs(b)
	n := (a.BitLen() + 31) / 32
	r := 0.0
	var d big.Int
	for i := n - 1; i >= 0; i-- {
		d.Rsh(&a, uint(32*i))
		r = r*(1<<32) + float64(d.Uint64()&0xffffffff)
	}
	if math.IsInf(r, 0) {
		return 0, false
	}
	if b.Sign() < 0 {
		r = -r
	}
	return r, true
}

// realToInteger truncates f to an integer, small or large, as iconx's
// cnv:integer does; it fails for infinities and NaN.
func realToInteger(f float64) (Value, bool) {
	if i, ok := realToInt(f); ok {
		return i, true
	}
	if math.IsInf(f, 0) || math.IsNaN(f) {
		return nil, false
	}
	b, _ := new(big.Float).SetFloat64(f).Int(nil)
	return mkInteger(b), true
}

// CvInteger converts v to an integer, small or large (iconx's
// cnv:integer), if it can.
func CvInteger(v Value) (Value, bool) {
	n, ok := CvNum(v)
	if !ok {
		return nil, false
	}
	if r, isReal := n.(Real); isReal {
		return realToInteger(float64(r))
	}
	return n, true
}

// MustInteger converts v to an integer, small or large, or raises
// error 101.
func MustInteger(v Value) Value {
	i, ok := CvInteger(v)
	if !ok {
		Runerr(101, v)
	}
	return i
}

// maxDigits is iconx's MaxDigits: image() shows a large integer with
// more (estimated) digits as integer(~10^n).
const maxDigits = 30

// bigImage returns the image of a large integer, as iconx's image()
// and bigprint: its digits, or integer(~10^n) if iconx's estimate of
// the number of digits, made from its base 2^32 digits, is maxDigits or
// more.
func bigImage(b *big.Int) string {
	var a big.Int
	a.Abs(b)
	slen := (a.BitLen()+31)/32 - 1 // index of the most significant digit
	var msd big.Int
	msd.Rsh(&a, uint(32*slen))
	dlen := int64(float64(slen*32)*0.3010299956639812 +
		math.Log(float64(msd.Uint64()))*0.4342944819032518 + 0.5)
	if dlen >= maxDigits {
		return "integer(~10^" + strconv.FormatInt(dlen, 10) + ")"
	}
	return b.String()
}

// Integer arithmetic. Each operation on Ints reports whether the result
// fits; if it does not, the caller redoes it with big.Int.

func addInt(x, y Int) (Int, bool) {
	s := x + y
	return s, (s > x) == (y > 0)
}

func subInt(x, y Int) (Int, bool) {
	d := x - y
	return d, (d < x) == (y > 0)
}

func mulInt(x, y Int) (Int, bool) {
	if x == 0 || y == 0 {
		return 0, true
	}
	p := x * y
	return p, p/y == x && !(x == -1 && y == math.MinInt64) && !(y == -1 && x == math.MinInt64)
}

func divInt(x, y Int) (Int, bool) {
	if y == 0 {
		Runerr(201)
	}
	if x == math.MinInt64 && y == -1 {
		return 0, false
	}
	return x / y, true
}

func modInt(x, y Int) (Int, bool) {
	if y == 0 {
		Runerr(202)
	}
	if y == -1 {
		return 0, true
	}
	return x % y, true
}

func addBig(x, y *big.Int) *big.Int { return new(big.Int).Add(x, y) }
func subBig(x, y *big.Int) *big.Int { return new(big.Int).Sub(x, y) }
func mulBig(x, y *big.Int) *big.Int { return new(big.Int).Mul(x, y) }

func divBig(x, y *big.Int) *big.Int {
	if y.Sign() == 0 {
		Runerr(201)
	}
	return new(big.Int).Quo(x, y) // truncated, as C's /
}

func modBig(x, y *big.Int) *big.Int {
	if y.Sign() == 0 {
		Runerr(202)
	}
	return new(big.Int).Rem(x, y) // the sign of x, as C's %
}

// intArith applies an integer operation to integers x and y.
func intArith(x, y Value, fi func(x, y Int) (Int, bool), fb func(x, y *big.Int) *big.Int) Value {
	if xi, ok := x.(Int); ok {
		if yi, ok := y.(Int); ok {
			if r, ok := fi(xi, yi); ok {
				return r
			}
		}
	}
	return mkInteger(fb(bigOf(x), bigOf(y)))
}

// neg returns -x for integer x.
func neg(x Value) Value {
	return intArith(Int(0), x, subInt, subBig)
}

// cmpInteger compares integers x and y.
func cmpInteger(x, y Value) int {
	if xi, ok := x.(Int); ok {
		if yi, ok := y.(Int); ok {
			return cmp64(int64(xi), int64(yi))
		}
	}
	return bigOf(x).Cmp(bigOf(y))
}

// intPow returns x ^ y for integers, as iconx's bigpow and bigpowii:
// exact. For y <= 0: 0 ^ y is error 204 (0 ^ 0 included), x ^ 0 is 1,
// 1 ^ y is 1, -1 ^ y is 1 or -1, and anything else is 0.
func intPow(x, y Value) Value {
	yb := bigOf(y)
	one := func(x Value, n int) bool { xi, ok := x.(Int); return ok && xi == Int(n) }
	switch {
	case one(x, 0) && yb.Sign() <= 0:
		Runerr(204)
	case yb.Sign() == 0, one(x, 1):
		return Int(1)
	case one(x, -1):
		if yb.Bit(0) == 0 {
			return Int(1)
		}
		return Int(-1)
	case yb.Sign() < 0:
		return Int(0)
	case one(x, 0):
		return Int(0)
	}
	if _, small := y.(Int); !small {
		// Any other base to a large power exhausts memory, as in iconx
		// (error 307).
		Runerr(307)
	}
	if xi, ok := x.(Int); ok {
		if r, ok := powInt(xi, int64(y.(Int))); ok {
			return r
		}
	}
	return mkInteger(new(big.Int).Exp(bigOf(x), yb, nil))
}

// powInt computes x ^ n for n > 0, reporting whether it fits in an Int.
func powInt(x Int, n int64) (Int, bool) {
	r := Int(1)
	for n > 0 {
		if n&1 != 0 {
			var ok bool
			if r, ok = mulInt(r, x); !ok {
				return 0, false
			}
		}
		n >>= 1
		if n > 0 {
			var ok bool
			if x, ok = mulInt(x, x); !ok {
				return 0, false
			}
		}
	}
	return r, true
}

// realPowBig raises a real to a large integer power, as iconx's
// bigpowri: square and multiply over the exponent's bits. Like
// bigpowri, it does not check the result, so 2.0 ^ (2 ^ 100) is inf.0
// where ordinary real overflow is error 204.
func realPowBig(a float64, b *big.Int) Real {
	if b.Sign() < 0 {
		if a == 0 {
			Runerr(204)
		}
		a = 1 / a
	}
	var m big.Int
	m.Abs(b)
	// iconx scans whole base 2^32 digits from the top, so leading zero
	// bits square 1.0, which changes nothing.
	r := 1.0
	for i := m.BitLen() - 1; i >= 0; i-- {
		r *= r
		if m.Bit(i) != 0 {
			r *= a
		}
	}
	return Real(r)
}

// Bitwise functions, on two's complement values of unbounded width, as
// iconx's (fmisc.r and rlrgint.r).
func init() {
	bitwise := func(name string, fi func(x, y Int) Int, fb func(z, x, y *big.Int) *big.Int) {
		fn(name, func(vm *VM, a []Value) (Value, bool) {
			x, y := MustInteger(arg(a, 0)), MustInteger(arg(a, 1))
			if xi, ok := x.(Int); ok {
				if yi, ok := y.(Int); ok {
					return fi(xi, yi), true
				}
			}
			return mkInteger(fb(new(big.Int), bigOf(x), bigOf(y))), true
		})
	}
	bitwise("iand", func(x, y Int) Int { return x & y }, (*big.Int).And)
	bitwise("ior", func(x, y Int) Int { return x | y }, (*big.Int).Or)
	bitwise("ixor", func(x, y Int) Int { return x ^ y }, (*big.Int).Xor)
	fn("icom", func(vm *VM, a []Value) (Value, bool) {
		x := MustInteger(arg(a, 0))
		if xi, ok := x.(Int); ok {
			return ^xi, true
		}
		return mkInteger(new(big.Int).Not(bigOf(x))), true
	})
	// ishift(i, j) is i * 2^j, rounded down: bits shifted out on the
	// right are lost, and a negative i shifts in ones.
	fn("ishift", func(vm *VM, a []Value) (Value, bool) {
		x, y := MustInteger(arg(a, 0)), MustInteger(arg(a, 1))
		j, small := y.(Int)
		if !small {
			Runerr(101, y)
		}
		if xi, ok := x.(Int); ok && j <= 0 {
			if j <= -64 {
				if xi < 0 {
					return Int(-1), true
				}
				return Int(0), true
			}
			return xi >> uint(-j), true
		}
		if j >= 0 {
			if j > 1<<32 {
				Runerr(307)
			}
			return mkInteger(new(big.Int).Lsh(bigOf(x), uint(j))), true
		}
		return mkInteger(new(big.Int).Rsh(bigOf(x), uint(-j))), true
	})
}

// NewInteger returns b as an Icon integer (for the compiler's
// constants). b must not be modified afterwards.
func NewInteger(b *big.Int) Value { return mkInteger(b) }
