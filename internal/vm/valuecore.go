package vm

import (
	"fmt"
	"math"
	"unsafe"
)

// A Value is an Icon value, or a variable, as a tagged struct
// (docs/design.md, "Decision: values as tagged structs"): integers,
// reals and &null are held in it, everything else by pointer, so that
// making a number or a variable allocates nothing.
type Value struct {
	k Kind
	n uint64         // an Int, a Real's bits, or an index
	p unsafe.Pointer // the object, or nil
}

// Kind is the kind of a Value.
type Kind uint8

const (
	KNone Kind = iota // no value (the zero Value), distinct from &null
	KNull
	KInt  // n
	KReal // n holds the bits
	KBig  // *Big
	KString
	KCset
	KList
	KTable
	KSet
	KRecord
	KProc
	KBuiltin
	KRecordType
	KCoexp
	KFile

	// Variables.
	KRef      // *Value: a local or global
	KListElem // *List, n the index from 0
	KField    // *Record, n the field's index
	KSubstr   // *SubstrVar
	KVar      // *varBox: any other Var (keywords, table elements)

	// Internal: what the VM keeps in temporaries.
	KFrame   // *Frame: a suspended procedure call
	KGen     // *genBox: a built-in generator
	KScanEnv // *scanEnv
	KPoison  // a released argument (GCONDEBUG=poisonargs)
)

// NullValue is &null.
var NullValue = Value{k: KNull}

// Kind returns v's kind.
func (v Value) Kind() Kind { return v.k }

// IsNone reports whether v is no value (the zero Value).
func (v Value) IsNone() bool { return v.k == KNone }

// IsNull reports whether v is &null.
func (v Value) IsNull() bool { return v.k == KNull }

// IntVal returns i as a Value.
func IntVal(i Int) Value { return Value{k: KInt, n: uint64(i)} }

// RealVal returns r as a Value.
func RealVal(r Real) Value { return Value{k: KReal, n: math.Float64bits(float64(r))} }

// StrVal returns s as a Value.
func StrVal(s *String) Value { return Value{k: KString, p: unsafe.Pointer(s)} }

// ListVal returns l as a Value.
func ListVal(l *List) Value { return Value{k: KList, p: unsafe.Pointer(l)} }

// RefVal returns the variable whose value is in *p.
func RefVal(p *Value) Value { return Value{k: KRef, p: unsafe.Pointer(p)} }

// Int returns v's integer; v must be an Int.
func (v Value) Int() Int { return Int(v.n) }

// Real returns v's real; v must be a Real.
func (v Value) Real() Real { return Real(math.Float64frombits(v.n)) }

// Str returns v's string; v must be a string.
func (v Value) Str() *String { return (*String)(v.p) }

// List returns v's list; v must be a list.
func (v Value) List() *List { return (*List)(v.p) }

// IsInt reports whether v is an Int, returning it.
func (v Value) IsInt() (Int, bool) { return Int(v.n), v.k == KInt }

// IsStr reports whether v is a string, returning it.
func (v Value) IsStr() (*String, bool) { return (*String)(v.p), v.k == KString }

// varBox holds a variable of a kind the Value cannot hold directly.
type varBox struct{ v Var }

// genBox holds a built-in generator in a temporary.
type genBox struct{ g Generator }

// V returns the Value of x, a value in the representation the VM used
// before Values were structs: Null, Int, Real, a pointer to an object,
// a *Value, or a variable, generator or other thing a temporary holds.
// It is for code off the VM's fast paths; those build Values directly.
func V(x any) Value {
	switch x := x.(type) {
	case nil:
		return Value{}
	case Value:
		return x
	case Null:
		return NullValue
	case Int:
		return IntVal(x)
	case Real:
		return RealVal(x)
	case *Big:
		return Value{k: KBig, p: unsafe.Pointer(x)}
	case *String:
		return StrVal(x)
	case *Cset:
		return Value{k: KCset, p: unsafe.Pointer(x)}
	case *List:
		return ListVal(x)
	case *Table:
		return Value{k: KTable, p: unsafe.Pointer(x)}
	case *Set:
		return Value{k: KSet, p: unsafe.Pointer(x)}
	case *Record:
		return Value{k: KRecord, p: unsafe.Pointer(x)}
	case *Proc:
		return Value{k: KProc, p: unsafe.Pointer(x)}
	case *Builtin:
		return Value{k: KBuiltin, p: unsafe.Pointer(x)}
	case *RecordType:
		return Value{k: KRecordType, p: unsafe.Pointer(x)}
	case *Coexp:
		return Value{k: KCoexp, p: unsafe.Pointer(x)}
	case *File:
		return Value{k: KFile, p: unsafe.Pointer(x)}
	case *Value:
		return RefVal(x)
	case ListElem:
		return Value{k: KListElem, n: uint64(x.I), p: unsafe.Pointer(x.L)}
	case FieldVar:
		return Value{k: KField, n: uint64(x.I), p: unsafe.Pointer(x.R)}
	case *SubstrVar:
		return Value{k: KSubstr, p: unsafe.Pointer(x)}
	case *Frame:
		return Value{k: KFrame, p: unsafe.Pointer(x)}
	case *scanEnv:
		return Value{k: KScanEnv, p: unsafe.Pointer(x)}
	case poisoned:
		return Value{k: KPoison}
	case Var:
		return Value{k: KVar, p: unsafe.Pointer(&varBox{x})}
	case Generator:
		return Value{k: KGen, p: unsafe.Pointer(&genBox{x})}
	}
	panic(fmt.Sprintf("vm: V of %T", x))
}

// Any returns v in the representation V takes. Like V, it is for code
// off the fast paths: an Int or Real it returns is boxed again.
func (v Value) Any() any {
	switch v.k {
	case KNone:
		return nil
	case KNull:
		return Null{}
	case KInt:
		return Int(v.n)
	case KReal:
		return v.Real()
	case KBig:
		return (*Big)(v.p)
	case KString:
		return (*String)(v.p)
	case KCset:
		return (*Cset)(v.p)
	case KList:
		return (*List)(v.p)
	case KTable:
		return (*Table)(v.p)
	case KSet:
		return (*Set)(v.p)
	case KRecord:
		return (*Record)(v.p)
	case KProc:
		return (*Proc)(v.p)
	case KBuiltin:
		return (*Builtin)(v.p)
	case KRecordType:
		return (*RecordType)(v.p)
	case KCoexp:
		return (*Coexp)(v.p)
	case KFile:
		return (*File)(v.p)
	case KRef:
		return (*Value)(v.p)
	case KListElem:
		return ListElem{(*List)(v.p), int(v.n)}
	case KField:
		return FieldVar{(*Record)(v.p), int(v.n)}
	case KSubstr:
		return (*SubstrVar)(v.p)
	case KVar:
		return (*varBox)(v.p).v
	case KFrame:
		return (*Frame)(v.p)
	case KGen:
		return (*genBox)(v.p).g
	case KScanEnv:
		return (*scanEnv)(v.p)
	case KPoison:
		return poisoned{}
	}
	panic(fmt.Sprintf("vm: bad kind %d", v.k))
}

// IsVar reports whether v is a variable.
func (v Value) IsVar() bool { return v.k >= KRef && v.k <= KVar }

// Deref returns the value of v if it is a variable, else v.
func Deref(v Value) Value {
	switch v.k {
	case KRef:
		return *(*Value)(v.p)
	case KListElem:
		l := (*List)(v.p)
		if int(v.n) < len(l.Elems) {
			return l.Elems[v.n]
		}
		return NullValue
	case KField:
		return (*Record)(v.p).Fields[v.n]
	case KSubstr:
		return (*SubstrVar)(v.p).Get()
	case KVar:
		return (*varBox)(v.p).v.Get()
	}
	return v
}

// asVar returns v as a Var, if it is a variable.
func asVar(v Value) (Var, bool) {
	switch v.k {
	case KRef:
		return refVar{(*Value)(v.p)}, true
	case KListElem:
		return ListElem{(*List)(v.p), int(v.n)}, true
	case KField:
		return FieldVar{(*Record)(v.p), int(v.n)}, true
	case KSubstr:
		return (*SubstrVar)(v.p), true
	case KVar:
		return (*varBox)(v.p).v, true
	}
	return nil, false
}

// refVar is a local or global as a Var.
type refVar struct{ p *Value }

func (r refVar) Get() Value       { return *r.p }
func (r refVar) Set(v Value) bool { *r.p = v; return true }
