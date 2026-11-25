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

// Constructors and accessors for the other kinds. An accessor assumes v
// is of its kind.

func BigVal(b *Big) Value               { return Value{k: KBig, p: unsafe.Pointer(b)} }
func CsetVal(c *Cset) Value             { return Value{k: KCset, p: unsafe.Pointer(c)} }
func TableVal(t *Table) Value           { return Value{k: KTable, p: unsafe.Pointer(t)} }
func SetVal(s *Set) Value               { return Value{k: KSet, p: unsafe.Pointer(s)} }
func RecordVal(r *Record) Value         { return Value{k: KRecord, p: unsafe.Pointer(r)} }
func ProcVal(p *Proc) Value             { return Value{k: KProc, p: unsafe.Pointer(p)} }
func BuiltinVal(b *Builtin) Value       { return Value{k: KBuiltin, p: unsafe.Pointer(b)} }
func RecordTypeVal(t *RecordType) Value { return Value{k: KRecordType, p: unsafe.Pointer(t)} }
func CoexpVal(c *Coexp) Value           { return Value{k: KCoexp, p: unsafe.Pointer(c)} }
func FileVal(f *File) Value             { return Value{k: KFile, p: unsafe.Pointer(f)} }
func frameVal(f *Frame) Value           { return Value{k: KFrame, p: unsafe.Pointer(f)} }
func genVal(g Generator) Value          { return Value{k: KGen, p: unsafe.Pointer(&genBox{g})} }
func scanEnvVal(e *scanEnv) Value       { return Value{k: KScanEnv, p: unsafe.Pointer(e)} }
func listElemVal(l *List, i int) Value {
	return Value{k: KListElem, n: uint64(i), p: unsafe.Pointer(l)}
}
func fieldVal(r *Record, i int) Value   { return Value{k: KField, n: uint64(i), p: unsafe.Pointer(r)} }
func (v Value) Big() *Big               { return (*Big)(v.p) }
func (v Value) Cset() *Cset             { return (*Cset)(v.p) }
func (v Value) Table() *Table           { return (*Table)(v.p) }
func (v Value) Set() *Set               { return (*Set)(v.p) }
func (v Value) Record() *Record         { return (*Record)(v.p) }
func (v Value) Proc() *Proc             { return (*Proc)(v.p) }
func (v Value) Builtin() *Builtin       { return (*Builtin)(v.p) }
func (v Value) RecordType() *RecordType { return (*RecordType)(v.p) }
func (v Value) Coexp() *Coexp           { return (*Coexp)(v.p) }
func (v Value) File() *File             { return (*File)(v.p) }
func (v Value) frame() *Frame           { return (*Frame)(v.p) }
func (v Value) gen() Generator          { return (*genBox)(v.p).g }
func (v Value) scanEnv() *scanEnv       { return (*scanEnv)(v.p) }
func (v Value) ref() *Value             { return (*Value)(v.p) }
func (v Value) substr() *SubstrVar      { return (*SubstrVar)(v.p) }

// assignVar assigns x to the variable dst, reporting whether the
// assignment succeeded and whether dst is a variable at all.
func assignVar(dst, x Value) (ok, isVar bool) {
	switch dst.k {
	case KRef:
		*(*Value)(dst.p) = x
		return true, true
	case KListElem:
		if l := (*List)(dst.p); int(dst.n) < len(l.Elems) {
			l.Elems[dst.n] = x
		}
		return true, true
	case KField:
		(*Record)(dst.p).Fields[dst.n] = x
		return true, true
	case KSubstr:
		return (*SubstrVar)(dst.p).Set(x), true
	case KVar:
		return (*varBox)(dst.p).v.Set(x), true
	}
	return false, false
}

// As... report whether v is of a kind, returning its object; they take
// the place of type assertions on the old interface Values.

func (v Value) AsInt() (Int, bool) {
	if v.k != KInt {
		var zero Int
		return zero, false
	}
	return Int(v.n), true
}

func (v Value) AsReal() (Real, bool) {
	if v.k != KReal {
		var zero Real
		return zero, false
	}
	return v.Real(), true
}

func (v Value) AsNull() (Null, bool) {
	if v.k != KNull {
		var zero Null
		return zero, false
	}
	return Null{}, true
}

func (v Value) AsString() (*String, bool) {
	if v.k != KString {
		var zero *String
		return zero, false
	}
	return (*String)(v.p), true
}

func (v Value) AsBig() (*Big, bool) {
	if v.k != KBig {
		var zero *Big
		return zero, false
	}
	return (*Big)(v.p), true
}

func (v Value) AsCset() (*Cset, bool) {
	if v.k != KCset {
		var zero *Cset
		return zero, false
	}
	return (*Cset)(v.p), true
}

func (v Value) AsList() (*List, bool) {
	if v.k != KList {
		var zero *List
		return zero, false
	}
	return (*List)(v.p), true
}

func (v Value) AsTable() (*Table, bool) {
	if v.k != KTable {
		var zero *Table
		return zero, false
	}
	return (*Table)(v.p), true
}

func (v Value) AsSetObj() (*Set, bool) {
	if v.k != KSet {
		var zero *Set
		return zero, false
	}
	return (*Set)(v.p), true
}

func (v Value) AsRecord() (*Record, bool) {
	if v.k != KRecord {
		var zero *Record
		return zero, false
	}
	return (*Record)(v.p), true
}

func (v Value) AsProc() (*Proc, bool) {
	if v.k != KProc {
		var zero *Proc
		return zero, false
	}
	return (*Proc)(v.p), true
}

func (v Value) AsBuiltin() (*Builtin, bool) {
	if v.k != KBuiltin {
		var zero *Builtin
		return zero, false
	}
	return (*Builtin)(v.p), true
}

func (v Value) AsRecordType() (*RecordType, bool) {
	if v.k != KRecordType {
		var zero *RecordType
		return zero, false
	}
	return (*RecordType)(v.p), true
}

func (v Value) AsCoexp() (*Coexp, bool) {
	if v.k != KCoexp {
		var zero *Coexp
		return zero, false
	}
	return (*Coexp)(v.p), true
}

func (v Value) AsFile() (*File, bool) {
	if v.k != KFile {
		var zero *File
		return zero, false
	}
	return (*File)(v.p), true
}

func (v Value) AsRef() (*Value, bool) {
	if v.k != KRef {
		var zero *Value
		return zero, false
	}
	return (*Value)(v.p), true
}

func (v Value) AsFrame() (*Frame, bool) {
	if v.k != KFrame {
		var zero *Frame
		return zero, false
	}
	return (*Frame)(v.p), true
}

func (v Value) AsSubstr() (*SubstrVar, bool) {
	if v.k != KSubstr {
		var zero *SubstrVar
		return zero, false
	}
	return (*SubstrVar)(v.p), true
}

func (v Value) AsScanEnv() (*scanEnv, bool) {
	if v.k != KScanEnv {
		var zero *scanEnv
		return zero, false
	}
	return (*scanEnv)(v.p), true
}

func (v Value) AsListElem() (ListElem, bool) {
	if v.k != KListElem {
		var zero ListElem
		return zero, false
	}
	return ListElem{(*List)(v.p), int(v.n)}, true
}

func (v Value) AsFieldVar() (FieldVar, bool) {
	if v.k != KField {
		var zero FieldVar
		return zero, false
	}
	return FieldVar{(*Record)(v.p), int(v.n)}, true
}

// listElem and fieldVar return a KListElem or KField Value's variable.
func (v Value) listElem() ListElem { return ListElem{(*List)(v.p), int(v.n)} }
func (v Value) fieldVar() FieldVar { return FieldVar{(*Record)(v.p), int(v.n)} }

// substrVal returns the substring variable s as a Value.
func substrVal(s *SubstrVar) Value { return Value{k: KSubstr, p: unsafe.Pointer(s)} }

// varVal returns a variable of a kind the Value cannot hold directly.
func varVal(v Var) Value { return Value{k: KVar, p: unsafe.Pointer(&varBox{v})} }
