package vm

import (
	"fmt"
	"sort"
	"sync"
)

// A built program names its built-in functions and operators rather than
// holding Go function values (docs/design.md, "The built program's
// format"). Every *Builtin the compiler puts in a program comes from one
// of the tables below, so a (kind, name) pair identifies it, and decoding
// the pair yields the same pointer: the VM compares some builtins by
// identity (ApplyOp, the AsgnOp markers).

// BuiltinKind says which table a builtin comes from.
type BuiltinKind uint8

const (
	BuiltinOp          BuiltinKind = 0 // Ops[name]: operators and keywords, by their Ops key ("u-", "&pos")
	BuiltinFunction    BuiltinKind = 1 // Functions[name]
	BuiltinAsgn        BuiltinKind = 2 // AsgnOp(name)
	BuiltinApply       BuiltinKind = 3 // ApplyOp; the name is empty
	BuiltinField       BuiltinKind = 4 // FieldOp(name)
	BuiltinUnsupported BuiltinKind = 5 // Unsupported(name)

	numBuiltinKinds = 6
)

var builtinKindNames = [numBuiltinKinds]string{"op", "function", "asgn", "apply", "field", "unsupported"}

func (k BuiltinKind) String() string {
	if k < numBuiltinKinds {
		return builtinKindNames[k]
	}
	return fmt.Sprintf("kind%d", k)
}

// BuiltinRef identifies a builtin.
type BuiltinRef struct {
	Kind BuiltinKind
	Name string
}

func (r BuiltinRef) String() string {
	if r.Kind == BuiltinApply {
		return "apply"
	}
	return r.Kind.String() + " " + r.Name
}

// memo is a table of builtins made on demand, one per name, so that the
// same name always gives the same builtin.
type memo struct {
	mu sync.Mutex
	m  map[string]*Builtin
	mk func(name string) *Builtin
}

func newMemo(mk func(string) *Builtin) *memo {
	return &memo{m: map[string]*Builtin{}, mk: mk}
}

func (t *memo) get(name string) *Builtin {
	t.mu.Lock()
	defer t.mu.Unlock()
	b := t.m[name]
	if b == nil {
		b = t.mk(name)
		t.m[name] = b
	}
	return b
}

// find returns the name under which t holds b.
func (t *memo) find(b *Builtin) (string, bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	for n, x := range t.m {
		if x == b {
			return n, true
		}
	}
	return "", false
}

// opKeys maps each operator to its key in Ops, built once Ops is complete
// (it is filled by package initialisation). Where one builtin has
// several keys, the least is used.
var opKeys = sync.OnceValue(func() map[*Builtin]string {
	keys := make([]string, 0, len(Ops))
	for k := range Ops {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	m := map[*Builtin]string{}
	for _, k := range keys {
		if _, ok := m[Ops[k]]; !ok {
			m[Ops[k]] = k
		}
	}
	return m
})

// RefOf returns the reference that identifies b.
func RefOf(b *Builtin) (BuiltinRef, bool) {
	switch {
	case b == ApplyOp:
		return BuiltinRef{BuiltinApply, ""}, true
	case Functions[b.Name] == b:
		return BuiltinRef{BuiltinFunction, b.Name}, true
	}
	if k, ok := opKeys()[b]; ok {
		return BuiltinRef{BuiltinOp, k}, true
	}
	for _, t := range []struct {
		kind BuiltinKind
		m    *memo
	}{{BuiltinAsgn, asgnOps}, {BuiltinField, fieldOps}, {BuiltinUnsupported, unsupported}} {
		if n, ok := t.m.find(b); ok {
			return BuiltinRef{t.kind, n}, true
		}
	}
	return BuiltinRef{}, false
}

// Resolve returns the builtin r identifies.
func (r BuiltinRef) Resolve() (*Builtin, bool) {
	switch r.Kind {
	case BuiltinOp:
		b, ok := Ops[r.Name]
		return b, ok
	case BuiltinFunction:
		b, ok := Functions[r.Name]
		return b, ok
	case BuiltinAsgn:
		return AsgnOp(r.Name), true
	case BuiltinApply:
		return ApplyOp, r.Name == ""
	case BuiltinField:
		return FieldOp(r.Name), true
	case BuiltinUnsupported:
		return Unsupported(r.Name), true
	}
	return nil, false
}
