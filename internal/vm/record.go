package vm

import "strconv"

// RecordType is a record declaration: its constructor is a procedure.
type RecordType struct {
	Name   string
	Fields []string
	index  map[string]int
	serial int // last serial number issued
}

// NewRecordType returns the type for record name(fields...).
func NewRecordType(name string, fields []string) *RecordType {
	t := &RecordType{Name: name, Fields: fields, index: map[string]int{}}
	for i, f := range fields {
		if _, dup := t.index[f]; !dup {
			t.index[f] = i
		}
	}
	return t
}

// Record is a record value.
type Record struct {
	T      *RecordType
	Fields []Value
	serial int
}

// construct makes a record from the constructor's arguments: missing
// fields are &null, and extra arguments are ignored.
func (t *RecordType) construct(args []Value) *Record {
	t.serial++
	r := &Record{T: t, Fields: make([]Value, len(t.Fields)), serial: t.serial}
	for i := range r.Fields {
		r.Fields[i] = arg(args, i)
	}
	return r
}

// FieldVar is the variable r.f.
type FieldVar struct {
	R *Record
	I int
}

func (v FieldVar) Get() Value       { return v.R.Fields[v.I] }
func (v FieldVar) Set(x Value) bool { v.R.Fields[v.I] = x; return true }

func (r *Record) image() string {
	return "record " + r.T.Name + "_" + strconv.Itoa(r.serial) + "(" + strconv.Itoa(len(r.Fields)) + ")"
}

// FieldOp returns the operator x.name.
func FieldOp(name string) *Builtin {
	return &Builtin{Name: "." + name, Fn: func(vm *VM, a []Value) (Value, bool) {
		r, ok := a[0].(*Record)
		if !ok {
			Runerr(107, a[0])
		}
		i, ok := r.T.index[name]
		if !ok {
			Runerr(207, r)
		}
		return FieldVar{r, i}, true
	}}
}

// recordIndex is r[x]: by position, or by field name.
func recordIndex(r *Record, x Value) (Value, bool) {
	if s, ok := x.(*String); ok {
		if _, isNum := strToNum(s.s); !isNum {
			i, ok := r.T.index[s.s]
			return FieldVar{r, i}, ok
		}
	}
	i := MustInt(x)
	n := Int(len(r.Fields))
	if i <= 0 {
		i += n + 1
	}
	if i < 1 || i > n {
		return nil, false
	}
	return FieldVar{r, int(i - 1)}, true
}

type recordGen struct {
	r *Record
	i int
}

func (g *recordGen) Next(vm *VM) (Value, bool) {
	if g.i >= len(g.r.Fields) {
		return nil, false
	}
	g.i++
	return FieldVar{g.r, g.i - 1}, true
}
