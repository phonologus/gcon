package vm

import "unsafe"

// Generators in Values. A KGen Value holds the generator's pointer in p
// and a number naming its type in n, so that holding one allocates
// nothing and Next is a direct call; a generator of a type not listed
// here is boxed (genOther).
//
// A funcGen, a closure, is held by its closure pointer: a Go func value
// is one pointer to its closure, as the runtime and reflect have it
// (TestFuncGenInValue checks that it round-trips).

const (
	genOther   = iota // p is a *genBox
	genFunc           // p is a funcGen's closure
	genFail           // failGen
	genRestore        // restore &pos to n >> 8 and fail (restoring)
	gen_toGen
	gen_listGen
	gen_strGen
	gen_recordGen
	gen_restorer
	gen_omapGen
	gen_seqGen
	gen_strVarGen
	gen_findGen
	gen_uptoGen
	gen_stringsGen
	gen_revGen
	gen_intsGen
)

// genVal returns g as a Value.
func genVal(g Generator) Value {
	switch g := g.(type) {
	case funcGen:
		return Value{k: KGen, n: genFunc, p: *(*unsafe.Pointer)(unsafe.Pointer(&g))}
	case failGen:
		return Value{k: KGen, n: genFail}
	case *toGen:
		return Value{k: KGen, n: gen_toGen, p: unsafe.Pointer(g)}
	case *listGen:
		return Value{k: KGen, n: gen_listGen, p: unsafe.Pointer(g)}
	case *strGen:
		return Value{k: KGen, n: gen_strGen, p: unsafe.Pointer(g)}
	case *recordGen:
		return Value{k: KGen, n: gen_recordGen, p: unsafe.Pointer(g)}
	case *restorer:
		return Value{k: KGen, n: gen_restorer, p: unsafe.Pointer(g)}
	case *omapGen:
		return Value{k: KGen, n: gen_omapGen, p: unsafe.Pointer(g)}
	case *seqGen:
		return Value{k: KGen, n: gen_seqGen, p: unsafe.Pointer(g)}
	case *strVarGen:
		return Value{k: KGen, n: gen_strVarGen, p: unsafe.Pointer(g)}
	case *uptoGen:
		return Value{k: KGen, n: gen_uptoGen, p: unsafe.Pointer(g)}
	case *findGen:
		return Value{k: KGen, n: gen_findGen, p: unsafe.Pointer(g)}
	case *stringsGen:
		return Value{k: KGen, n: gen_stringsGen, p: unsafe.Pointer(g)}
	case *revGen:
		return Value{k: KGen, n: gen_revGen, p: unsafe.Pointer(g)}
	case *intsGen:
		return Value{k: KGen, n: gen_intsGen, p: unsafe.Pointer(g)}
	}
	return Value{k: KGen, n: genOther, p: unsafe.Pointer(&genBox{g})}
}

// gen returns the generator a KGen Value holds.
func (v Value) gen() Generator {
	switch v.n & 0xff { // the low byte: a restoring handle keeps &pos above it
	case genFunc:
		return funcOf(v)
	case genFail:
		return failGen{}
	case genRestore:
		return &restorer{old: int(v.n >> 8), done: true}
	case gen_toGen:
		return (*toGen)(v.p)
	case gen_listGen:
		return (*listGen)(v.p)
	case gen_strGen:
		return (*strGen)(v.p)
	case gen_recordGen:
		return (*recordGen)(v.p)
	case gen_restorer:
		return (*restorer)(v.p)
	case gen_omapGen:
		return (*omapGen)(v.p)
	case gen_seqGen:
		return (*seqGen)(v.p)
	case gen_strVarGen:
		return (*strVarGen)(v.p)
	case gen_uptoGen:
		return (*uptoGen)(v.p)
	case gen_findGen:
		return (*findGen)(v.p)
	case gen_stringsGen:
		return (*stringsGen)(v.p)
	case gen_revGen:
		return (*revGen)(v.p)
	case gen_intsGen:
		return (*intsGen)(v.p)
	}
	return (*genBox)(v.p).g
}

// genNext produces the next result of the generator a KGen Value holds.
func genNext(v Value, vm *VM) (Value, bool) {
	if v.n == gen_toGen { // the commonest, driving every i := m to n
		return (*toGen)(v.p).Next(vm)
	}
	switch v.n & 0xff { // the low byte: a restoring handle keeps &pos above it
	case genFunc:
		return funcOf(v)()
	case genFail:
		return Value{}, false
	case genRestore:
		vm.pos = int(v.n >> 8)
		return Value{}, false
	case gen_toGen:
		return (*toGen)(v.p).Next(vm)
	case gen_listGen:
		return (*listGen)(v.p).Next(vm)
	case gen_strGen:
		return (*strGen)(v.p).Next(vm)
	case gen_recordGen:
		return (*recordGen)(v.p).Next(vm)
	case gen_restorer:
		return (*restorer)(v.p).Next(vm)
	case gen_omapGen:
		return (*omapGen)(v.p).Next(vm)
	case gen_seqGen:
		return (*seqGen)(v.p).Next(vm)
	case gen_strVarGen:
		return (*strVarGen)(v.p).Next(vm)
	case gen_uptoGen:
		return (*uptoGen)(v.p).Next(vm)
	case gen_findGen:
		return (*findGen)(v.p).Next(vm)
	case gen_stringsGen:
		return (*stringsGen)(v.p).Next(vm)
	case gen_revGen:
		return (*revGen)(v.p).Next(vm)
	case gen_intsGen:
		return (*intsGen)(v.p).Next(vm)
	}
	return (*genBox)(v.p).g.Next(vm)
}

// funcOf returns the funcGen a genFunc Value holds.
func funcOf(v Value) funcGen {
	var f funcGen
	*(*unsafe.Pointer)(unsafe.Pointer(&f)) = v.p
	return f
}
