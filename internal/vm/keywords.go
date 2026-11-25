package vm

import (
	"fmt"
	"os"
	"strings"
	"time"
)

// The remaining keywords, as iconx's keyword.r: dates and times, the
// host, the program name, the version, &dump, the storage keywords,
// and the event and graphics keywords as they are in an Icon without
// graphics.

// Version is &version.
const Version = "Icon Version 9.5 (gcon)"

// The storage keywords describe iconx's memory regions and garbage
// collections, which gcon does not have (its memory is Go's): &regions
// gives iconx's initial region sizes and &storage and &allocated zeros,
// so that programs printing them run the same. &collections counts the
// collections programs ask for with collect(), which are the only ones
// in a program that does not fill iconx's regions.
var storageKeywords = map[string][]Int{
	"&regions":   {0, 500000, 500000}, // static, string, block
	"&storage":   {0, 0, 0},
	"&allocated": {0, 0, 0, 0}, // total, static, string, block
}

// MouseEvents are the event-code keywords, constants in every Icon.
var MouseEvents = map[string]Int{
	"lpress": -1, "mpress": -2, "rpress": -3,
	"lrelease": -4, "mrelease": -5, "rrelease": -6,
	"ldrag": -7, "mdrag": -8, "rdrag": -9, "resize": -10,
}

// valueVar is a keyword variable holding any value (&eventcode and the
// like).
type valueVar struct{ p *Value }

func (k valueVar) Get() Value       { return *k.p }
func (k valueVar) Set(v Value) bool { *k.p = v; return true }

// prognameVar is &progname: assignments are converted to strings.
type prognameVar struct{ vm *VM }

func (k prognameVar) Get() Value { return V(k.vm.progname) }
func (k prognameVar) Set(v Value) bool {
	k.vm.progname = MustStr(v)
	return true
}

// dumpVar is &dump: while nonzero when the program ends, the
// identifiers are listed on &errout ("Termination dump").
type dumpVar struct{ vm *VM }

func (k dumpVar) Get() Value { return V(k.vm.dump) }
func (k dumpVar) Set(v Value) bool {
	k.vm.dump = MustInt(v)
	return true
}

// SetProgname sets &progname from the path of the program's source:
// the name the program would have if built by icont and run from its
// directory ("./kwds" for kwds.icn), as iconx's is its argv[0].
func (vm *VM) SetProgname(path string) {
	name := strings.TrimSuffix(path, ".icn")
	if !strings.Contains(name, "/") {
		name = "./" + name
	}
	vm.progname = Text(name)
}

// SetInvokedName sets &progname to name exactly: for a built program,
// the name it was invoked by, as iconx's &progname is the icode file's
// name as the shell header passes it.
func (vm *VM) SetInvokedName(name string) { vm.progname = Text(name) }

// termDump writes iconx's termination dump if &dump is nonzero.
func (vm *VM) termDump() {
	if vm.dump == 0 {
		return
	}
	vm.out.Flush()
	fmt.Fprintf(vm.errout, "\nTermination dump:\n\nco-expression #%d(%d)\n", vm.co.serial, vm.co.results)
	vm.errout.WriteString(vm.listing(vm.level))
	vm.errout.Flush()
}

func init() {
	kw := func(name string, f func(vm *VM) Value) {
		Ops["&"+name] = &Builtin{Name: "&" + name, Fn: func(vm *VM, a []Value) (Value, bool) {
			return f(vm), true
		}}
	}
	kw("clock", func(vm *VM) Value { return V(Text(time.Now().Format("15:04:05"))) })
	kw("date", func(vm *VM) Value { return V(Text(time.Now().Format("2006/01/02"))) })
	kw("dateline", func(vm *VM) Value {
		// As iconx's: "Monday, March 8, 2026  3:04 pm", the hour 1..12.
		t := time.Now()
		h, merid := t.Hour(), "am"
		if h >= 12 {
			merid = "pm"
			if h > 12 {
				h -= 12
			}
		} else if h < 1 {
			h += 12
		}
		return V(Text(fmt.Sprintf("%s, %s %d, %d  %d:%02d %s", t.Weekday(), t.Month(),
			t.Day(), t.Year(), h, t.Minute(), merid)))
	})
	kw("time", func(vm *VM) Value { return V(Int(cpuMillis())) })
	kw("host", func(vm *VM) Value {
		h, _ := os.Hostname()
		return V(Text(h))
	})
	kw("version", func(vm *VM) Value { return V(Text(Version)) })
	kw("progname", func(vm *VM) Value { return V(prognameVar{vm}) })
	kw("dump", func(vm *VM) Value { return V(dumpVar{vm}) })
	kw("eventcode", func(vm *VM) Value { return V(valueVar{&vm.eventcode}) })
	kw("eventsource", func(vm *VM) Value { return V(valueVar{&vm.eventsource}) })
	kw("eventvalue", func(vm *VM) Value { return V(valueVar{&vm.eventvalue}) })
	Ops["&collections"] = &Builtin{Name: "&collections", Gen: func(vm *VM, a []Value) Generator {
		return &intsGen{append([]Int(nil), vm.collections[:]...)}
	}}
	for name, vals := range storageKeywords {
		vals := vals
		Ops[name] = &Builtin{Name: name, Gen: func(vm *VM, a []Value) Generator {
			return &intsGen{vals}
		}}
	}
}

type intsGen struct{ v []Int }

func (g *intsGen) Next(vm *VM) (Value, bool) {
	if len(g.v) == 0 {
		return Value{}, false
	}
	v := g.v[0]
	g.v = g.v[1:]
	return V(v), true
}
