package vm

import (
	"math"
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
			return nil, false
		}
		n, err := lit.ParseNumber(s)
		if err != nil || n.IsReal {
			return nil, false
		}
		if neg {
			return Int(-n.Int), true
		}
		return Int(n.Int), true
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
		return nil, false
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
			return nil, false
		}
	}
	if i != len(s) {
		return nil, false
	}
	if neg {
		s = "-" + s
	}
	if !real {
		v, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return nil, false
		}
		return Int(v), true
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil || math.IsInf(f, 0) {
		return nil, false
	}
	return Real(f), true
}

func isDigit(c byte) bool { return '0' <= c && c <= '9' }

// CvNum converts v to a number (Int or Real), if it can.
func CvNum(v Value) (Value, bool) {
	switch x := v.(type) {
	case Int, Real:
		return x, true
	case *String:
		return strToNum(x.s)
	case *Cset:
		return strToNum(x.String().s)
	}
	return nil, false
}

// CvReal converts v to a real, if it can.
func CvReal(v Value) (float64, bool) {
	n, ok := CvNum(v)
	if !ok {
		return 0, false
	}
	return toFloat(n), true
}

func toFloat(n Value) float64 {
	if i, ok := n.(Int); ok {
		return float64(i)
	}
	return float64(n.(Real))
}

// realToInt truncates f to an integer, if it is in range.
//
// TODO: large integers.
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
func arith(name string, fi func(x, y Int) Int, fr func(x, y float64) float64) *Builtin {
	return &Builtin{Name: name, Fn: func(vm *VM, a []Value) (Value, bool) {
		x, y := MustNum(a[0]), MustNum(a[1])
		if xi, ok := x.(Int); ok {
			if yi, ok := y.(Int); ok {
				return fi(xi, yi), true
			}
		}
		return realResult(fr(toFloat(x), toFloat(y))), true
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
	if yi, ok := y.(Int); ok {
		if xi, ok := x.(Int); ok {
			return pow(xi, yi), true
		}
		return ripow(float64(x.(Real)), int64(yi)), true
	}
	fx, fy := toFloat(x), toFloat(y)
	if fx == 0 && fy < 0 {
		Runerr(204)
	}
	if fx < 0 {
		Runerr(206)
	}
	return realResult(math.Pow(fx, fy)), true
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
		if xi, isInt := x.(Int); isInt {
			if yi, isInt := y.(Int); isInt {
				c := 0
				if xi < yi {
					c = -1
				} else if xi > yi {
					c = 1
				}
				return yi, ok(c)
			}
		}
		fx, fy := toFloat(x), toFloat(y)
		c := 0
		if fx < fy {
			c = -1
		} else if fx > fy {
			c = 1
		}
		return Real(fy), ok(c)
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
		return Real(f(x)), true
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
		if _, null := arg(a, 1).(Null); null {
			return Real(math.Atan(y)), true
		}
		return Real(math.Atan2(y, MustReal(a[1]))), true
	})
	fn("log", func(vm *VM, a []Value) (Value, bool) {
		x := MustReal(arg(a, 0))
		if x <= 0 {
			Runerr(205, a[0])
		}
		if _, null := arg(a, 1).(Null); null {
			return Real(math.Log(x)), true
		}
		b := MustReal(a[1])
		if b <= 1 {
			Runerr(205, a[1])
		}
		return Real(math.Log(x) / math.Log(b)), true
	})
	fn("abs", func(vm *VM, a []Value) (Value, bool) {
		switch n := MustNum(arg(a, 0)).(type) {
		case Int:
			if n < 0 {
				return sub(0, n), true
			}
			return n, true
		case Real:
			return Real(math.Abs(float64(n))), true
		}
		return nil, false
	})
	fn("real", func(vm *VM, a []Value) (Value, bool) {
		f, ok := CvReal(arg(a, 0))
		return Real(f), ok
	})
	fn("numeric", func(vm *VM, a []Value) (Value, bool) {
		return CvNum(arg(a, 0))
	})
}
