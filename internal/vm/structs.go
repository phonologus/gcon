package vm

import (
	"encoding/binary"
	"math/rand/v2"
	"sort"
	"strconv"
	"strings"
	"unsafe"
)

// Tables and sets. Keys and members are compared as === compares them:
// numbers, strings and csets by value; structures, procedures, files and
// co-expressions by identity.
//
// Iteration (!T, key(T), !S) starts at a random entry and goes round in
// insertion order, as Go randomises map iteration, so that programs do
// not come to depend on an order Icon leaves unspecified (iconx's is the
// order of its hash table); see docs/design.md. Set operations and sort
// use insertion order, so their results are deterministic.

// hkey is a map key with === semantics.
type hkey struct {
	t uint8
	i int64
	f float64
	s string
	p unsafe.Pointer // a structure, by identity
}

func keyOf(v Value) hkey {
	switch v.Kind() {
	case KNull:
		return hkey{t: 0}
	case KInt:
		x := v.Int()
		return hkey{t: 1, i: int64(x)}
	case KBig:
		x := v.Big()
		return hkey{t: 1, s: x.v.String()} // never equal to an Int's key
	case KReal:
		x := v.Real()
		f := float64(x)
		if f == 0 {
			f = 0 // -0.0 === 0.0
		}
		return hkey{t: 2, f: f}
	case KString:
		x := v.Str()
		k := hkey{t: 3, s: x.s}
		if !x.IsASCII() {
			k.i = int64(x.unit)
		}
		return k
	case KCset:
		x := v.Cset()
		var b strings.Builder
		for _, w := range x.bits {
			b.Write(binary.LittleEndian.AppendUint64(nil, w))
		}
		for _, r := range x.ranges {
			b.Write(binary.LittleEndian.AppendUint32(nil, uint32(r.lo)))
			b.Write(binary.LittleEndian.AppendUint32(nil, uint32(r.hi)))
		}
		return hkey{t: 4, s: b.String()}
	}
	return hkey{t: 5 + uint8(v.k), p: v.p}
}

// omap is an insertion-ordered map from Icon values.
type omap struct {
	index map[hkey]int // position in keys and vals
	keys  []Value
	vals  []Value
	dead  int // deleted entries (nil keys) not yet compacted
	// shared: a traversal holds a slice of keys, so existing slots must
	// not be rewritten (only blanked) until the arrays are replaced.
	shared bool
}

func newOmap() omap { return omap{index: map[hkey]int{}} }

func (m *omap) len() int { return len(m.index) }

func (m *omap) get(k Value) (Value, bool) {
	if i, ok := m.index[keyOf(k)]; ok {
		return m.vals[i], true
	}
	return Value{}, false
}

func (m *omap) put(k, v Value) {
	h := keyOf(k)
	if i, ok := m.index[h]; ok {
		m.vals[i] = v
		return
	}
	m.index[h] = len(m.keys)
	m.keys = append(m.keys, k)
	m.vals = append(m.vals, v)
}

func (m *omap) del(k Value) {
	h := keyOf(k)
	i, ok := m.index[h]
	if !ok {
		return
	}
	delete(m.index, h)
	if i == len(m.keys)-1 && !m.shared {
		// The last entry (often the one just inserted): drop it.
		m.keys, m.vals = m.keys[:i], m.vals[:i]
		return
	}
	m.keys[i], m.vals[i] = Value{}, Value{}
	m.dead++
	if m.dead > 16 && m.dead > len(m.keys)/2 {
		m.compact()
	}
}

// compact removes deleted entries. Generators in progress are not
// affected: they iterate over a snapshot of the keys.
// compact removes the deleted entries: in place, unless a traversal
// shares the arrays, when it makes new ones.
func (m *omap) compact() {
	if m.shared {
		keys, vals := make([]Value, 0, m.len()), make([]Value, 0, m.len())
		for i, k := range m.keys {
			if !k.IsNone() {
				m.index[keyOf(k)] = len(keys)
				keys = append(keys, k)
				vals = append(vals, m.vals[i])
			}
		}
		m.keys, m.vals, m.dead, m.shared = keys, vals, 0, false
		return
	}
	n := 0
	for i, k := range m.keys {
		if !k.IsNone() {
			if n != i {
				m.index[keyOf(k)] = n
				m.keys[n], m.vals[n] = k, m.vals[i]
			}
			n++
		}
	}
	clear(m.keys[n:])
	clear(m.vals[n:])
	m.keys, m.vals, m.dead = m.keys[:n], m.vals[:n], 0
}

func (m *omap) copy() omap {
	c := newOmap()
	for i, k := range m.keys {
		if !k.IsNone() {
			c.put(k, m.vals[i])
		}
	}
	return c
}

// Table is an Icon table.
type Table struct {
	m      omap
	dflt   Value
	serial int
}

// Set is an Icon set (an omap whose values are unused).
type Set struct {
	m      omap
	serial int
}

func (vm *VM) newTable(dflt Value) *Table {
	vm.serials.tables++
	return &Table{m: newOmap(), dflt: dflt, serial: vm.serials.tables}
}

func (vm *VM) newSet() *Set {
	vm.serials.sets++
	return &Set{m: newOmap(), serial: vm.serials.sets}
}

// TableElem is the variable T[k]: reading a missing key gives the
// table's default value; assigning adds the key.
type TableElem struct {
	T *Table
	K Value
}

func (e TableElem) Get() Value {
	if v, ok := e.T.m.get(e.K); ok {
		return v
	}
	return e.T.dflt
}

func (e TableElem) Set(v Value) bool { e.T.m.put(e.K, v); return true }

// omapGen generates the entries of an omap, each as made by f from its
// key. It works on a snapshot of the keys taken when it starts, from a
// random one round to the one before it, skipping keys deleted since; as
// with Go's maps, a key added meanwhile may not be produced.
type omapGen struct {
	m        *omap
	keys     []Value // the snapshot, taken on the first call
	i, start int
	started  bool
	f        func(k Value) Value
}

func (g *omapGen) Next(vm *VM) (Value, bool) {
	if !g.started {
		g.started = true
		g.keys = g.m.keys[:len(g.m.keys):len(g.m.keys)]
		g.m.shared = true
		if len(g.keys) > 0 {
			g.start = rand.IntN(len(g.keys))
		}
	}
	for g.i < len(g.keys) {
		k := g.keys[(g.start+g.i)%len(g.keys)]
		g.i++
		if k.IsNone() {
			continue
		}
		if _, ok := g.m.index[keyOf(k)]; ok {
			return g.f(k), true
		}
	}
	return Value{}, false
}

func tableVals(t *Table) Generator {
	return &omapGen{m: &t.m, f: func(k Value) Value { return varVal(TableElem{t, k}) }}
}

func setElems(s *Set) Generator {
	return &omapGen{m: &s.m, f: func(k Value) Value { return k }}
}

// members returns the live keys of m, in order.
func (m *omap) members() []Value {
	out := make([]Value, 0, m.len())
	for _, k := range m.keys {
		if !k.IsNone() {
			out = append(out, k)
		}
	}
	return out
}

// setOp computes a set union ('+'), intersection ('*') or difference ('-').
func (vm *VM) setOp(op byte, a, b *Set) *Set {
	r := vm.newSet()
	for _, x := range a.m.members() {
		_, inB := b.m.get(x)
		if op == '+' || (op == '*') == inB {
			r.m.put(x, NullValue)
		}
	}
	if op == '+' {
		for _, x := range b.m.members() {
			r.m.put(x, NullValue)
		}
	}
	return r
}

// ---------------------------------------------------------------------
// Sorting

// order is the collating class of a value, as iconx's order()
// (src/runtime/rcomp.r).
func order(v Value) int {
	switch v.Kind() {
	case KNull:
		return 0
	case KInt, KBig:
		return 1
	case KReal:
		return 2
	case KString:
		return 3
	case KCset:
		return 4
	case KFile:
		return 5
	case KCoexp:
		return 6
	case KProc, KBuiltin, KRecordType:
		return 7
	case KList:
		return 8
	case KSet:
		return 9
	case KTable:
		return 10
	case KRecord:
		return 11
	}
	return 12
}

func procName(v Value) string {
	switch v.Kind() {
	case KProc:
		p := v.Proc()
		return p.Name
	case KBuiltin:
		p := v.Builtin()
		return p.Name
	case KRecordType:
		p := v.RecordType()
		return p.Name
	}
	return ""
}

func cmpInt(a, b int) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// anycmp compares two values in Icon's sorting order, as iconx's anycmp:
// by collating class, then within a class by value, name or serial
// number.
func anycmp(x, y Value) int {
	ox, oy := order(x), order(y)
	if ox != oy {
		return cmpInt(ox, oy)
	}
	switch x.Kind() {
	case KInt, KBig:
		a := x.Any()
		return cmpInteger(V(a), y)
	case KReal:
		a := x.Real()
		b := y.Real()
		switch {
		case a < b:
			return -1
		case a > b:
			return 1
		}
		return 0
	case KString:
		a := x.Str()
		return strings.Compare(a.s, y.Str().s)
	case KCset:
		a := x.Cset()
		// As the strings the csets convert to compare.
		return strings.Compare(a.String().s, y.Cset().String().s)
	case KFile:
		a := x.File()
		return strings.Compare(a.name, y.File().name)
	case KCoexp:
		a := x.Coexp()
		return cmpInt(a.serial, y.Coexp().serial)
	case KList:
		a := x.List()
		return cmpInt(a.serial, y.List().serial)
	case KSet:
		a := x.Set()
		return cmpInt(a.serial, y.Set().serial)
	case KTable:
		a := x.Table()
		return cmpInt(a.serial, y.Table().serial)
	case KRecord:
		a := x.Record()
		b := y.Record()
		if c := strings.Compare(a.T.Name, b.T.Name); c != 0 {
			return c
		}
		return cmpInt(a.serial, b.serial)
	}
	if ox == 7 {
		return strings.Compare(procName(x), procName(y))
	}
	return 0
}

func cmp64(a, b int64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// nth returns the nth field of a record or element of a list, as iconx's
// nth does for sortf.
func nth(v Value, n Int) (Value, bool) {
	switch v.Kind() {
	case KRecord:
		x := v.Record()
		if i, ok := cvpos(n, len(x.Fields)); ok && i <= len(x.Fields) {
			return x.Fields[i-1], true
		}
	case KList:
		x := v.List()
		if i, ok := cvpos(n, len(x.Elems)); ok && i <= len(x.Elems) {
			return x.Elems[i-1], true
		}
	}
	return Value{}, false
}

// nthcmp compares on the nth field, as iconx's nthcmp: values with no
// such field come first, and ties are settled by anycmp.
func nthcmp(x, y Value, n Int) int {
	_, rx := x.AsRecord()
	_, lx := x.AsList()
	if (rx || lx) && order(x) == order(y) {
		ex, okx := nth(x, n)
		ey, oky := nth(y, n)
		switch {
		case !okx && oky:
			return -1
		case okx && !oky:
			return 1
		case okx && oky:
			if c := anycmp(ex, ey); c != 0 {
				return c
			}
		}
	}
	return anycmp(x, y)
}

// sortValues sorts vs by cmp. iconx uses qsort, which is not stable; gcon
// sorts stably, which differs only in the order of elements that compare
// equal.
func sortValues(vs []Value, cmp func(x, y Value) int) {
	sort.SliceStable(vs, func(i, j int) bool { return cmp(vs[i], vs[j]) < 0 })
}

// elements returns the values of a list, set or record, for sort.
func elements(v Value) ([]Value, bool) {
	switch v.Kind() {
	case KList:
		x := v.List()
		return append([]Value(nil), x.Elems...), true
	case KSet:
		x := v.Set()
		return x.m.members(), true
	case KRecord:
		x := v.Record()
		return append([]Value(nil), x.Fields...), true
	}
	return nil, false
}

func (t *Table) image() string {
	return "table_" + strconv.Itoa(t.serial) + "(" + strconv.Itoa(t.m.len()) + ")"
}

func (s *Set) image() string {
	return "set_" + strconv.Itoa(s.serial) + "(" + strconv.Itoa(s.m.len()) + ")"
}

// seqGen generates i, i+j, i+2j, ...
type seqGen struct{ i, j Int }

func (g *seqGen) Next(vm *VM) (Value, bool) {
	v := g.i
	g.i = add(g.i, g.j)
	return IntVal(v), true
}

func init() {
	fn("table", func(vm *VM, a []Value) (Value, bool) { return TableVal(vm.newTable(arg(a, 0))), true })
	fn("set", func(vm *VM, a []Value) (Value, bool) {
		s := vm.newSet()
		switch val := arg(a, 0); val.Kind() {
		case KNull:
		case KList:
			l := val.List()
			for _, x := range l.Elems {
				s.m.put(x, NullValue)
			}
		default:
			l := val.Any()
			Runerr(108, V(l))
		}
		return SetVal(s), true
	})
	fn("insert", func(vm *VM, a []Value) (Value, bool) {
		switch sval := arg(a, 0); sval.Kind() {
		case KSet:
			x := sval.Set()
			x.m.put(arg(a, 1), NullValue)
			return SetVal(x), true
		case KTable:
			x := sval.Table()
			x.m.put(arg(a, 1), arg(a, 2))
			return TableVal(x), true
		}
		Runerr(122, arg(a, 0))
		return Value{}, false
	})
	fn("delete", func(vm *VM, a []Value) (Value, bool) {
		switch val := arg(a, 0); val.Kind() {
		case KSet:
			x := val.Set()
			x.m.del(arg(a, 1))
			return SetVal(x), true
		case KTable:
			x := val.Table()
			x.m.del(arg(a, 1))
			return TableVal(x), true
		}
		Runerr(122, arg(a, 0))
		return Value{}, false
	})
	fn("member", func(vm *VM, a []Value) (Value, bool) {
		switch val := arg(a, 0); val.Kind() {
		case KSet:
			x := val.Set()
			_, ok := x.m.get(arg(a, 1))
			return arg(a, 1), ok
		case KTable:
			x := val.Table()
			_, ok := x.m.get(arg(a, 1))
			return arg(a, 1), ok
		}
		Runerr(122, arg(a, 0))
		return Value{}, false
	})
	gen("key", func(vm *VM, a []Value) Generator {
		t, ok := arg(a, 0).AsTable()
		if !ok {
			Runerr(124, arg(a, 0))
		}
		return &omapGen{m: &t.m, f: func(k Value) Value { return k }}
	})
	fn("sort", func(vm *VM, a []Value) (Value, bool) {
		if t, ok := arg(a, 0).AsTable(); ok {
			i := Int(1)
			if _, null := arg(a, 1).AsNull(); !null {
				i = MustInt(a[1])
			}
			if i < 1 || i > 4 {
				Runerr(205, IntVal(i))
			}
			// Sort [key, value] pairs; methods 1 and 2 return them as
			// lists, 3 and 4 as one alternating list (making no pair
			// lists, as iconx makes none, so serial numbers agree).
			var pairs [][2]Value
			for k, key := range t.m.keys {
				if !key.IsNone() {
					pairs = append(pairs, [2]Value{key, t.m.vals[k]})
				}
			}
			by := 0
			if i == 2 || i == 4 {
				by = 1
			}
			sort.SliceStable(pairs, func(x, y int) bool { return anycmp(pairs[x][by], pairs[y][by]) < 0 })
			// The result list is made first, so it has the lower serial
			// number, as in iconx.
			result := vm.NewList(nil)
			out := make([]Value, 0, 2*len(pairs))
			for _, p := range pairs {
				if i <= 2 {
					out = append(out, ListVal(vm.NewList([]Value{p[0], p[1]})))
				} else {
					out = append(out, p[0], p[1])
				}
			}
			result.Elems = out
			return ListVal(result), true
		}
		vs, ok := elements(arg(a, 0))
		if !ok {
			Runerr(115, arg(a, 0))
		}
		sortValues(vs, anycmp)
		return ListVal(vm.NewList(vs)), true
	})
	fn("sortf", func(vm *VM, a []Value) (Value, bool) {
		vs, ok := elements(arg(a, 0))
		if !ok {
			Runerr(125, arg(a, 0))
		}
		n := Int(1)
		if _, null := arg(a, 1).AsNull(); !null {
			n = MustInt(a[1])
		}
		if n == 0 {
			Runerr(205, IntVal(n))
		}
		sortValues(vs, func(x, y Value) int { return nthcmp(x, y, n) })
		return ListVal(vm.NewList(vs)), true
	})
	fn("copy", func(vm *VM, a []Value) (Value, bool) {
		switch val := arg(a, 0); val.Kind() {
		case KList:
			x := val.List()
			return ListVal(vm.NewList(append([]Value(nil), x.Elems...))), true
		case KTable:
			x := val.Table()
			t := vm.newTable(x.dflt)
			t.m = x.m.copy()
			return TableVal(t), true
		case KSet:
			x := val.Set()
			s := vm.newSet()
			s.m = x.m.copy()
			return SetVal(s), true
		case KRecord:
			x := val.Record()
			return RecordVal(x.T.construct(x.Fields)), true
		}
		return arg(a, 0), true
	})
	fn("serial", func(vm *VM, a []Value) (Value, bool) {
		switch val := arg(a, 0); val.Kind() {
		case KList:
			x := val.List()
			return IntVal(Int(x.serial)), true
		case KSet:
			x := val.Set()
			return IntVal(Int(x.serial)), true
		case KTable:
			x := val.Table()
			return IntVal(Int(x.serial)), true
		case KRecord:
			x := val.Record()
			return IntVal(Int(x.serial)), true
		case KCoexp:
			x := val.Coexp()
			return IntVal(Int(x.serial)), true
		}
		return Value{}, false
	})
	gen("seq", func(vm *VM, a []Value) Generator {
		g := &seqGen{i: 1, j: 1}
		if _, null := arg(a, 0).AsNull(); !null {
			g.i = MustInt(a[0])
		}
		if _, null := arg(a, 1).AsNull(); !null {
			g.j = MustInt(a[1])
		}
		if g.j == 0 {
			Runerr(211, IntVal(g.j))
		}
		return g
	})
}
