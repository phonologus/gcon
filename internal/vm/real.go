package vm

import (
	"math"
	"math/big"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/phonologus/gcon/internal/lit"
)

// Real is an Icon real number.
type Real float64

// rtos converts a real to a string as iconx's rtos does
// (src/runtime/cnv.r): C's "%.10g", with a ".0" added when there is
// neither a decimal point nor an exponent. Infinities are "inf.0" and
// "-inf.0", as C's printf and rtos make them.
func rtos(f float64) string {
	switch {
	case math.IsInf(f, 1):
		return "inf.0"
	case math.IsInf(f, -1):
		return "-inf.0"
	case math.IsNaN(f):
		return "nan.0"
	}
	s := strconv.FormatFloat(f+0, 'g', 10, 64) // +0 avoids "-0", as rtos does
	if !strings.ContainsAny(s, ".e") {
		s += ".0"
	}
	return s
}

// realResult returns f, or raises error 204 if it is not finite.
func realResult(f float64) Real {
	if math.IsInf(f, 0) || math.IsNaN(f) {
		Runerr(204)
	}
	return Real(f)
}

// strToNum converts a string to a number as Icon does: surrounding white
// space; an optional sign; then a radix integer (16rFF), or decimal digits
// with an optional fraction and exponent ("5.", ".5" and "1E3" are
// numbers; "1e", "0x10" and "inf" are not). An integer literal gives an
// integer, else a real.
//
// TODO: integers beyond 64 bits (large integers) are not yet numbers.
func strToNum(s string) (Value, bool) {
	s = strings.TrimFunc(s, func(r rune) bool { return r < utf8.RuneSelf && unicode.IsSpace(r) })
	neg := false
	if s != "" && (s[0] == '+' || s[0] == '-') {
		neg = s[0] == '-'
		s = s[1:]
	}
	if strings.ContainsAny(s, "rR") {
		if s == "" || s[0] < '0' || s[0] > '9' {
			return Value{}, false
		}
		n, err := lit.ParseNumber(s)
		if err != nil || n.IsReal {
			return Value{}, false
		}
		v := n.Big
		if v == nil {
			v = big.NewInt(n.Int)
		}
		if neg {
			v = new(big.Int).Neg(v)
		}
		return mkInteger(v), true
	}
	// digits [. digits] [e [sign] digits], with a digit before or after
	// the point.
	i, mant, real := 0, 0, false
	for i < len(s) && isDigit(s[i]) {
		i, mant = i+1, mant+1
	}
	if i < len(s) && s[i] == '.' {
		real = true
		for i++; i < len(s) && isDigit(s[i]); i++ {
			mant++
		}
	}
	if mant == 0 {
		return Value{}, false
	}
	if i < len(s) && (s[i] == 'e' || s[i] == 'E') {
		real = true
		i++
		if i < len(s) && (s[i] == '+' || s[i] == '-') {
			i++
		}
		exp := 0
		for ; i < len(s) && isDigit(s[i]); i++ {
			exp++
		}
		if exp == 0 {
			return Value{}, false
		}
	}
	if i != len(s) {
		return Value{}, false
	}
	if neg {
		s = "-" + s
	}
	if !real {
		if v, err := strconv.ParseInt(s, 10, 64); err == nil {
			return IntVal(Int(v)), true
		}
		b, ok := new(big.Int).SetString(s, 10)
		if !ok {
			return Value{}, false
		}
		return mkInteger(b), true
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil || math.IsInf(f, 0) {
		return Value{}, false
	}
	return RealVal(Real(f)), true
}

func isDigit(c byte) bool { return '0' <= c && c <= '9' }

// CvNum converts v to a number (Int or Real), if it can.
func CvNum(v Value) (Value, bool) {
	switch sv132 := v; sv132.Kind() {
	case KInt, KReal, KBig:
		x := sv132.Any()
		return V(x), true
	case KString:
		x := sv132.Str()
		return strToNum(x.s)
	case KCset:
		x := sv132.Cset()
		return strToNum(x.String().s)
	}
	return Value{}, false
}

// CvReal converts v to a real, if it can.
func CvReal(v Value) (float64, bool) {
	n, ok := CvNum(v)
	if !ok {
		return 0, false
	}
	if b, isBig := n.AsBig(); isBig {
		return bigToReal(b.v)
	}
	return toFloat(n), true
}

// toFloat converts number n to a real. A large integer too large for a
// real raises error 102, as in iconx where the conversion fails.
func toFloat(n Value) float64 {
	switch sv158 := n; sv158.Kind() {
	case KInt:
		x := sv158.Int()
		return float64(x)
	case KBig:
		x := sv158.Big()
		f, ok := bigToReal(x.v)
		if !ok {
			Runerr(102, BigVal(x))
		}
		return f
	}
	return float64(n.Real())
}

// realToInt truncates f to a word-sized integer, if it is in range
// (realToInteger allows large ones).
func realToInt(f float64) (Int, bool) {
	if math.IsNaN(f) || f >= 1<<63 || f < -(1<<63) {
		return 0, false
	}
	return Int(f), true
}

// MustNum converts v to a number or raises error 102.
func MustNum(v Value) Value {
	n, ok := CvNum(v)
	if !ok {
		Runerr(102, v)
	}
	return n
}

// MustReal converts v to a real or raises error 102.
func MustReal(v Value) float64 {
	f, ok := CvReal(v)
	if !ok {
		Runerr(102, v)
	}
	return f
}

// arith makes a binary arithmetic operator: integer arithmetic when both
// operands are integers, else real.
func arith(name string, fi func(x, y Int) (Int, bool), fb func(x, y *big.Int) *big.Int, fr func(x, y float64) float64) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := MustNum(a[0]), MustNum(a[1])
		if isInteger(x) && isInteger(y) {
			return intArith(x, y, fi, fb), true
		}
		return RealVal(realResult(fr(toFloat(x), toFloat(y)))), true
	}}
}

func rdiv(x, y float64) float64 {
	if y == 0 {
		Runerr(204)
	}
	return x / y
}

func rmod(x, y float64) float64 {
	if y == 0 {
		Runerr(204)
	}
	return math.Mod(x, y)
}

// power implements x ^ y as iconx's powr does (src/runtime/oarith.r): an
// integer exponent with an integer base gives an integer; with a real
// base, repeated squaring (ripow); a real exponent needs a non-negative
// base (else error 206).
func power(vm *VM, a []Value) (Value, bool) {
	x, y := MustNum(a[0]), MustNum(a[1])
	if isInteger(y) {
		if isInteger(x) {
			return intPow(x, y), true
		}
		if yi, ok := y.AsInt(); ok {
			return RealVal(ripow(float64(x.Real()), int64(yi))), true
		}
		return RealVal(realPowBig(float64(x.Real()), y.Big().v)), true
	}
	fx, fy := toFloat(x), toFloat(y)
	if fx == 0 && fy < 0 {
		Runerr(204)
	}
	if fx < 0 {
		Runerr(206)
	}
	return RealVal(realResult(math.Pow(fx, fy))), true
}

// ripow raises a real to an integral power, step for step as iconx's
// ripow does, so that results agree to the last bit.
func ripow(r float64, n int64) Real {
	if r == 0 && n <= 0 {
		Runerr(204)
	}
	ret := 1.0
	if n < 0 {
		n = -1 - n
		r = 1 / r
		ret = r
	}
	for n > 0 {
		if n&1 != 0 {
			ret *= r
		}
		r *= r
		n >>= 1
	}
	return realResult(ret)
}

// numCompare makes a numeric comparison. Its result is the right operand,
// converted to real if either operand is real.
func numCompare(name string, ok func(c int) bool) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := MustNum(a[0]), MustNum(a[1])
		if isInteger(x) && isInteger(y) {
			return y, ok(cmpInteger(x, y))
		}
		fx, fy := toFloat(x), toFloat(y)
		c := 0
		if fx < fy {
			c = -1
		} else if fx > fy {
			c = 1
		}
		return RealVal(Real(fy)), ok(c)
	}}
}

// mathFn makes a function of one real argument; bad reports arguments
// outside its domain (error 205).
func mathFn(name string, f func(float64) float64, bad func(float64) bool) {
	fn(name, func(vm *VM, a []Value) (Value, bool) {
		x := MustReal(arg(a, 0))
		if bad != nil && bad(x) {
			Runerr(205, a[0])
		}
		return RealVal(Real(f(x))), true
	})
}

func init() {
	notUnit := func(x float64) bool { return x < -1 || x > 1 }
	mathFn("sqrt", math.Sqrt, func(x float64) bool { return x < 0 })
	mathFn("exp", math.Exp, nil)
	mathFn("sin", math.Sin, nil)
	mathFn("cos", math.Cos, nil)
	mathFn("tan", math.Tan, nil)
	mathFn("asin", math.Asin, notUnit)
	mathFn("acos", math.Acos, notUnit)
	mathFn("dtor", func(x float64) float64 { return x * math.Pi / 180 }, nil)
	mathFn("rtod", func(x float64) float64 { return x * 180 / math.Pi }, nil)
	fn("atan", func(vm *VM, a []Value) (Value, bool) {
		y := MustReal(arg(a, 0))
		if _, null := arg(a, 1).AsNull(); null {
			return RealVal(Real(math.Atan(y))), true
		}
		return RealVal(Real(math.Atan2(y, MustReal(a[1])))), true
	})
	fn("log", func(vm *VM, a []Value) (Value, bool) {
		x := MustReal(arg(a, 0))
		if x <= 0 {
			Runerr(205, a[0])
		}
		if _, null := arg(a, 1).AsNull(); null {
			return RealVal(Real(math.Log(x))), true
		}
		b := MustReal(a[1])
		if b <= 1 {
			Runerr(205, a[1])
		}
		return RealVal(Real(math.Log(x) / math.Log(b))), true
	})
	fn("abs", func(vm *VM, a []Value) (Value, bool) {
		switch sv335 := MustNum(arg(a, 0)); sv335.Kind() {
		case KInt, KBig:
			n := sv335.Any()
			if cmpInteger(V(n), IntVal(Int(0))) < 0 {
				return neg(V(n)), true
			}
			return V(n), true
		case KReal:
			n := sv335.Real()
			return RealVal(Real(math.Abs(float64(n)))), true
		}
		return Value{}, false
	})
	fn("real", func(vm *VM, a []Value) (Value, bool) {
		f, ok := CvReal(arg(a, 0))
		return RealVal(Real(f)), ok
	})
	fn("numeric", func(vm *VM, a []Value) (Value, bool) {
		return CvNum(arg(a, 0))
	})
}
