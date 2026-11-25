package vm

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"math"
	"math/big"
	"sort"
)

// The built program's format (docs/design.md, "The built program's
// format"): Magic, the format version, then sections, each a four-byte
// tag, a length and that many bytes. A tag whose first letter is upper
// case is required: a reader that does not know it refuses the file. One
// whose first letter is lower case may be skipped. The required sections
// come in the order of requiredSections; the last section is "End ",
// holding the CRC-32 of everything before it.
//
// Integers are varints (encoding/binary) unless stated otherwise; a
// string is its length then its bytes.

// FormatVersion is the version of the format Encode writes and Decode
// reads. It changes with any change to the encoding, to the instruction
// set (instr.go), or to what an instruction does with its fields.
const FormatVersion = 1

// Magic begins an encoded program. As in PNG, the first byte is not
// ASCII and the line endings catch a file mangled as text.
const Magic = "\x89GCN\r\n\x1a\n"

const (
	tagBuiltins = "Bltn"
	tagRecords  = "Recd"
	tagProcs    = "Proc"
	tagConsts   = "Cnst"
	tagGlobals  = "Glob"
	tagInfo     = "info"
	tagEnd      = "End "
)

var requiredSections = []string{tagBuiltins, tagRecords, tagProcs, tagConsts, tagGlobals}

// Value tags.
const (
	valNull    = 0
	valInt     = 1 // signed varint
	valBig     = 2 // sign (0 or 1 for negative), magnitude as big-endian bytes
	valReal    = 3 // IEEE 754 bits, eight bytes little-endian
	valString  = 4 // unit, bytes
	valCset    = 5 // four 64-bit words little-endian, then the ranges, count and (lo, hi) pairs
	valBuiltin = 6 // index in the builtins section
	valProc    = 7 // index in the procedures section
	valRecord  = 8 // index in the record types section
)

// Instruction field flags: a field is present when nonzero (an operand
// when it has a kind, Args when it has elements).
const (
	instrDst = 1 << iota
	instrX
	instrH
	instrArgs
	instrFn
	instrL1
	instrL2
	instrFile
)

type encoder struct {
	b        []byte
	builtins map[*Builtin]int
	records  map[*RecordType]int
	procs    map[*Proc]int
}

func (e *encoder) uint(x uint64)  { e.b = binary.AppendUvarint(e.b, x) }
func (e *encoder) int(x int64)    { e.b = binary.AppendVarint(e.b, x) }
func (e *encoder) byte(x byte)    { e.b = append(e.b, x) }
func (e *encoder) u64(x uint64)   { e.b = binary.LittleEndian.AppendUint64(e.b, x) }
func (e *encoder) bytes(s []byte) { e.uint(uint64(len(s))); e.b = append(e.b, s...) }
func (e *encoder) str(s string)   { e.uint(uint64(len(s))); e.b = append(e.b, s...) }
func (e *encoder) strs(s []string) {
	e.uint(uint64(len(s)))
	for _, x := range s {
		e.str(x)
	}
}

// Encode returns the encoding of p, with build information info (shown by
// a disassembly, ignored when running).
func Encode(p *Program, info map[string]string) ([]byte, error) {
	e := &encoder{builtins: map[*Builtin]int{}, records: map[*RecordType]int{}, procs: map[*Proc]int{}}
	// Number the builtins, record types and procedures in the order
	// they are first met, so that the encoding is deterministic.
	var builtins []*Builtin
	var records []*RecordType
	var procs []*Proc
	addBuiltin := func(b *Builtin) {
		if _, ok := e.builtins[b]; !ok {
			e.builtins[b] = len(builtins)
			builtins = append(builtins, b)
		}
	}
	collect := func(v Value) {
		switch sv107 := v; sv107.Kind() {
		case KBuiltin:
			v := sv107.Builtin()
			addBuiltin(v)
		case KRecordType:
			v := sv107.RecordType()
			if _, ok := e.records[v]; !ok {
				e.records[v] = len(records)
				records = append(records, v)
			}
		case KProc:
			v := sv107.Proc()
			if _, ok := e.procs[v]; !ok {
				e.procs[v] = len(procs)
				procs = append(procs, v)
			}
		}
	}
	for _, v := range p.Globals {
		collect(v)
	}
	for _, v := range p.Consts {
		collect(v)
	}
	for _, pr := range procs {
		for i := range pr.Code {
			if f := pr.Code[i].Fn; f != nil {
				addBuiltin(f)
			}
		}
	}

	e.b = append(e.b, Magic...)
	e.uint(FormatVersion)
	if len(info) > 0 {
		keys := make([]string, 0, len(info))
		for k := range info {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		start := e.begin(tagInfo)
		e.uint(uint64(len(keys)))
		for _, k := range keys {
			e.str(k)
			e.str(info[k])
		}
		e.end(start)
	}

	start := e.begin(tagBuiltins)
	e.uint(uint64(len(builtins)))
	for _, b := range builtins {
		r, ok := RefOf(b)
		if !ok {
			return nil, fmt.Errorf("vm: builtin %q has no reference", b.Name)
		}
		e.byte(byte(r.Kind))
		e.str(r.Name)
	}
	e.end(start)

	start = e.begin(tagRecords)
	e.uint(uint64(len(records)))
	for _, r := range records {
		e.str(r.Name)
		e.strs(r.Fields)
	}
	e.end(start)

	start = e.begin(tagProcs)
	e.uint(uint64(len(procs)))
	for _, pr := range procs {
		if err := e.proc(pr); err != nil {
			return nil, err
		}
	}
	e.end(start)

	start = e.begin(tagConsts)
	e.uint(uint64(len(p.Consts)))
	for _, v := range p.Consts {
		if err := e.value(v); err != nil {
			return nil, err
		}
	}
	e.end(start)

	start = e.begin(tagGlobals)
	e.uint(uint64(len(p.Globals)))
	for i, v := range p.Globals {
		e.str(p.GlobalNames[i])
		if err := e.value(v); err != nil {
			return nil, err
		}
	}
	e.int(int64(p.Main))
	if p.Invocable == nil {
		e.byte(0) // all
	} else {
		e.byte(1)
		bits := make([]byte, (len(p.Invocable)+7)/8)
		for i, ok := range p.Invocable {
			if ok {
				bits[i/8] |= 1 << (i % 8)
			}
		}
		e.b = append(e.b, bits...)
	}
	e.end(start)

	crc := crc32.ChecksumIEEE(e.b)
	e.b = append(e.b, tagEnd...)
	e.uint(4)
	e.b = binary.LittleEndian.AppendUint32(e.b, crc)
	return e.b, nil
}

// begin starts a section: its tag, then a placeholder for its length,
// which end fills in.
func (e *encoder) begin(tag string) int {
	e.b = append(e.b, tag...)
	return len(e.b)
}

func (e *encoder) end(start int) {
	body := append([]byte(nil), e.b[start:]...)
	e.b = binary.AppendUvarint(e.b[:start], uint64(len(body)))
	e.b = append(e.b, body...)
}

func (e *encoder) operand(o Operand) error {
	if o.Index < 0 {
		return fmt.Errorf("vm: negative operand index %d", o.Index)
	}
	e.uint(uint64(o.Index)<<3 | uint64(o.Kind))
	return nil
}

func (e *encoder) proc(p *Proc) error {
	e.str(p.Name)
	e.str(p.File)
	e.uint(uint64(p.NParams))
	if p.Variadic {
		e.byte(1)
	} else {
		e.byte(0)
	}
	e.uint(uint64(p.NLocals))
	e.strs(p.LocalNames)
	e.uint(uint64(len(p.Statics)))
	for _, s := range p.Statics {
		e.uint(uint64(s))
	}
	e.strs(p.Files)
	e.uint(uint64(p.NTemps))
	e.uint(uint64(len(p.Code)))
	line := int32(0)
	for i := range p.Code {
		in := &p.Code[i]
		var flags byte
		if in.Dst.Kind != NoOperand {
			flags |= instrDst
		}
		if in.X.Kind != NoOperand {
			flags |= instrX
		}
		if in.H != 0 {
			flags |= instrH
		}
		if len(in.Args) > 0 {
			flags |= instrArgs
		}
		if in.Fn != nil {
			flags |= instrFn
		}
		if in.L1 != 0 {
			flags |= instrL1
		}
		if in.L2 != 0 {
			flags |= instrL2
		}
		if in.File != 0 {
			flags |= instrFile
		}
		e.byte(byte(in.Op))
		e.byte(flags)
		e.int(int64(in.Line - line))
		line = in.Line
		if flags&instrDst != 0 {
			if err := e.operand(in.Dst); err != nil {
				return err
			}
		}
		if flags&instrX != 0 {
			if err := e.operand(in.X); err != nil {
				return err
			}
		}
		if flags&instrH != 0 {
			e.uint(uint64(in.H))
		}
		if flags&instrArgs != 0 {
			e.uint(uint64(len(in.Args)))
			for _, a := range in.Args {
				if err := e.operand(a); err != nil {
					return err
				}
			}
		}
		if flags&instrFn != 0 {
			e.uint(uint64(e.builtins[in.Fn]))
		}
		if flags&instrL1 != 0 {
			e.uint(uint64(in.L1))
		}
		if flags&instrL2 != 0 {
			e.uint(uint64(in.L2))
		}
		if flags&instrFile != 0 {
			e.uint(uint64(in.File))
		}
	}
	return nil
}

func (e *encoder) value(v Value) error {
	switch sv330 := v; sv330.Kind() {
	case KNull:
		e.byte(valNull)
	case KInt:
		v := sv330.Int()
		e.byte(valInt)
		e.int(int64(v))
	case KBig:
		v := sv330.Big()
		e.byte(valBig)
		if v.v.Sign() < 0 {
			e.byte(1)
		} else {
			e.byte(0)
		}
		e.bytes(v.v.Bytes())
	case KReal:
		v := sv330.Real()
		e.byte(valReal)
		e.u64(math.Float64bits(float64(v)))
	case KString:
		v := sv330.Str()
		e.byte(valString)
		e.byte(byte(v.unit))
		e.str(v.s)
	case KCset:
		v := sv330.Cset()
		e.byte(valCset)
		for _, w := range v.bits {
			e.u64(w)
		}
		e.uint(uint64(len(v.ranges)))
		for _, r := range v.ranges {
			e.uint(uint64(r.lo))
			e.uint(uint64(r.hi))
		}
	case KBuiltin:
		v := sv330.Builtin()
		e.byte(valBuiltin)
		e.uint(uint64(e.builtins[v]))
	case KProc:
		v := sv330.Proc()
		e.byte(valProc)
		e.uint(uint64(e.procs[v]))
	case KRecordType:
		v := sv330.RecordType()
		e.byte(valRecord)
		e.uint(uint64(e.records[v]))
	default:
		v := sv330.Any()
		return fmt.Errorf("vm: cannot encode a constant of type %T", v)
	}
	return nil
}

// FormatError reports a file that is not a program this gcon can read.
type FormatError struct{ Msg string }

func (e *FormatError) Error() string { return e.Msg }

// Info is a built program's build information.
type Info map[string]string

// Decode returns the program encoded in b, with its build information.
// It checks the file thoroughly enough that a damaged one, or one built
// by another version of gcon, is reported rather than crashing the VM;
// it is not a defence against a crafted file, which is a program and
// can do as it likes anyway.
func Decode(b []byte) (*Program, Info, error) {
	if len(b) < len(Magic) || string(b[:len(Magic)]) != Magic {
		return nil, nil, &FormatError{"not a gcon program"}
	}
	d := &decoder{b: b, pos: len(Magic)}
	v := d.uint()
	if d.err != nil {
		return nil, nil, &FormatError{"damaged program: " + d.err.Error()}
	}
	if v != FormatVersion {
		who := "an older"
		if v > FormatVersion {
			who = "a newer"
		}
		return nil, nil, &FormatError{fmt.Sprintf("program built by %s gcon (format %d; this gcon reads format %d): rebuild it", who, v, FormatVersion)}
	}
	p, info, err := d.sections()
	if err != nil {
		return nil, nil, &FormatError{"damaged program: " + err.Error()}
	}
	if err := Validate(p); err != nil {
		return nil, nil, &FormatError{"damaged program: " + err.Error()}
	}
	return p, info, nil
}

type decoder struct {
	b   []byte
	pos int
	err error

	builtins []*Builtin
	records  []*RecordType
	procs    []*Proc
}

var errShort = errors.New("unexpected end of data")

func (d *decoder) fail(format string, args ...any) {
	if d.err == nil {
		d.err = fmt.Errorf(format, args...)
	}
}

func (d *decoder) uint() uint64 {
	if d.err != nil {
		return 0
	}
	x, n := binary.Uvarint(d.b[d.pos:])
	if n <= 0 {
		d.err = errShort
		return 0
	}
	d.pos += n
	return x
}

func (d *decoder) int() int64 {
	if d.err != nil {
		return 0
	}
	x, n := binary.Varint(d.b[d.pos:])
	if n <= 0 {
		d.err = errShort
		return 0
	}
	d.pos += n
	return x
}

// small reads an unsigned integer that must fit in an int32.
func (d *decoder) small() int {
	x := d.uint()
	if x > math.MaxInt32 {
		d.fail("integer %d out of range", x)
		return 0
	}
	return int(x)
}

// count reads a number of elements, each at least one byte long, so no
// more than there are bytes left.
func (d *decoder) count() int {
	n := d.small()
	if n > len(d.b)-d.pos {
		d.fail("count %d exceeds the data", n)
		return 0
	}
	return n
}

func (d *decoder) byte() byte {
	if d.err != nil {
		return 0
	}
	if d.pos >= len(d.b) {
		d.err = errShort
		return 0
	}
	d.pos++
	return d.b[d.pos-1]
}

func (d *decoder) take(n int) []byte {
	if d.err != nil {
		return nil
	}
	if n < 0 || n > len(d.b)-d.pos {
		d.err = errShort
		return nil
	}
	d.pos += n
	return d.b[d.pos-n : d.pos]
}

func (d *decoder) u64() uint64 {
	s := d.take(8)
	if s == nil {
		return 0
	}
	return binary.LittleEndian.Uint64(s)
}

func (d *decoder) str() string { return string(d.take(d.small())) }

func (d *decoder) strs() []string {
	n := d.count()
	if n == 0 {
		return nil
	}
	s := make([]string, n)
	for i := range s {
		s[i] = d.str()
	}
	return s
}

// sections decodes the sections that follow the version.
func (d *decoder) sections() (*Program, Info, error) {
	p := &Program{}
	var info Info
	next := 0 // index in requiredSections of the next required section
	for {
		start := d.pos
		tag := string(d.take(4))
		n := d.small()
		body := d.take(n)
		if d.err != nil {
			return nil, nil, d.err
		}
		if tag == tagEnd {
			if next < len(requiredSections) {
				return nil, nil, fmt.Errorf("missing section %q", requiredSections[next])
			}
			if n != 4 {
				return nil, nil, fmt.Errorf("bad %q section", tagEnd)
			}
			if crc32.ChecksumIEEE(d.b[:start]) != binary.LittleEndian.Uint32(body) {
				return nil, nil, errors.New("checksum mismatch")
			}
			if d.pos != len(d.b) {
				return nil, nil, errors.New("data after the end")
			}
			return p, info, nil
		}
		if tag[0] >= 'a' && tag[0] <= 'z' {
			if tag == tagInfo {
				info = Info{}
				s := &decoder{b: body}
				for i, k := 0, s.count(); i < k; i++ {
					key := s.str()
					info[key] = s.str()
				}
				if err := s.finish(tag); err != nil {
					return nil, nil, err
				}
			}
			continue // a section this gcon need not understand
		}
		if next >= len(requiredSections) || tag != requiredSections[next] {
			for _, t := range requiredSections {
				if tag == t {
					return nil, nil, fmt.Errorf("section %q out of order", tag)
				}
			}
			return nil, nil, fmt.Errorf("unknown section %q (built by a newer gcon?)", tag)
		}
		next++
		s := &decoder{b: body, builtins: d.builtins, records: d.records, procs: d.procs}
		switch tag {
		case tagBuiltins:
			s.builtinsSection()
			d.builtins = s.builtins
		case tagRecords:
			s.recordsSection()
			d.records = s.records
		case tagProcs:
			s.procsSection()
			d.procs = s.procs
		case tagConsts:
			for i, k := 0, s.count(); i < k; i++ {
				p.Consts = append(p.Consts, s.value())
			}
		case tagGlobals:
			s.globalsSection(p)
		}
		if err := s.finish(tag); err != nil {
			return nil, nil, err
		}
	}
}

// finish checks that a section decoded without error and was consumed.
func (d *decoder) finish(tag string) error {
	if d.err != nil {
		return fmt.Errorf("section %q: %v", tag, d.err)
	}
	if d.pos != len(d.b) {
		return fmt.Errorf("section %q: %d bytes left over", tag, len(d.b)-d.pos)
	}
	return nil
}

func (d *decoder) builtinsSection() {
	for i, n := 0, d.count(); i < n && d.err == nil; i++ {
		r := BuiltinRef{Kind: BuiltinKind(d.byte()), Name: d.str()}
		if d.err != nil {
			return
		}
		b, ok := r.Resolve()
		if !ok {
			d.fail("unknown builtin %s (built by a newer gcon?)", r)
			return
		}
		d.builtins = append(d.builtins, b)
	}
}

func (d *decoder) recordsSection() {
	for i, n := 0, d.count(); i < n && d.err == nil; i++ {
		name := d.str()
		d.records = append(d.records, NewRecordType(name, d.strs()))
	}
}

func (d *decoder) procsSection() {
	for i, n := 0, d.count(); i < n && d.err == nil; i++ {
		d.procs = append(d.procs, d.proc())
	}
}

func (d *decoder) globalsSection(p *Program) {
	n := d.count()
	for i := 0; i < n && d.err == nil; i++ {
		p.GlobalNames = append(p.GlobalNames, d.str())
		p.Globals = append(p.Globals, d.value())
	}
	p.Main = int(d.int())
	switch d.byte() {
	case 0:
	case 1:
		bits := d.take((n + 7) / 8)
		if bits != nil {
			p.Invocable = make([]bool, n)
			for i := range p.Invocable {
				p.Invocable[i] = bits[i/8]&(1<<(i%8)) != 0
			}
		}
	default:
		d.fail("bad invocable flag")
	}
}

func (d *decoder) operand() Operand {
	x := d.uint()
	k := OperandKind(x & 7)
	if k == NoOperand || k > ConstOp {
		d.fail("bad operand kind %d", k)
	}
	if x>>3 > math.MaxInt32 {
		d.fail("operand index out of range")
	}
	return Operand{Kind: k, Index: int32(x >> 3)}
}

func (d *decoder) proc() *Proc {
	p := &Proc{Name: d.str(), File: d.str()}
	p.NParams = d.small()
	switch d.byte() {
	case 0:
	case 1:
		p.Variadic = true
	default:
		d.fail("bad variadic flag")
	}
	p.NLocals = d.small()
	p.LocalNames = d.strs()
	if n := d.count(); n > 0 {
		p.Statics = make([]int, n)
		for i := range p.Statics {
			p.Statics[i] = d.small()
		}
	}
	p.Files = d.strs()
	p.NTemps = d.small()
	n := d.count()
	p.Code = make([]Instr, n)
	line := int64(0)
	for i := 0; i < n && d.err == nil; i++ {
		in := &p.Code[i]
		in.Op = Op(d.byte())
		flags := d.byte()
		line += d.int()
		if line < math.MinInt32 || line > math.MaxInt32 {
			d.fail("line out of range")
		}
		in.Line = int32(line)
		if flags&instrDst != 0 {
			in.Dst = d.operand()
		}
		if flags&instrX != 0 {
			in.X = d.operand()
		}
		if flags&instrH != 0 {
			in.H = int32(d.small())
		}
		if flags&instrArgs != 0 {
			in.Args = make([]Operand, d.count())
			for j := range in.Args {
				in.Args[j] = d.operand()
			}
		}
		if flags&instrFn != 0 {
			j := d.small()
			if j >= len(d.builtins) {
				d.fail("builtin %d out of range", j)
				return p
			}
			in.Fn = d.builtins[j]
		}
		if flags&instrL1 != 0 {
			in.L1 = int32(d.small())
		}
		if flags&instrL2 != 0 {
			in.L2 = int32(d.small())
		}
		if flags&instrFile != 0 {
			in.File = int32(d.small())
		}
	}
	return p
}

func (d *decoder) value() Value {
	switch t := d.byte(); t {
	case valNull:
		return NullValue
	case valInt:
		return IntVal(Int(d.int()))
	case valBig:
		neg := d.byte()
		x := new(big.Int).SetBytes(d.take(d.small()))
		if neg == 1 {
			x.Neg(x)
		} else if neg != 0 {
			d.fail("bad sign")
		}
		return NewInteger(x)
	case valReal:
		return RealVal(Real(math.Float64frombits(d.u64())))
	case valString:
		u := Unit(d.byte())
		if u != TextUnit && u != BytesUnit {
			d.fail("bad string unit %d", u)
		}
		return StrVal(NewString(d.str(), u))
	case valCset:
		c := &Cset{n: -1}
		for i := range c.bits {
			c.bits[i] = d.u64()
		}
		if n := d.count(); n > 0 {
			c.ranges = make([]runeRange, n)
			prev := rune(0x7F)
			for i := range c.ranges {
				lo, hi := d.uint(), d.uint()
				if lo > hi || hi > 0x10FFFF || rune(lo) <= prev {
					d.fail("bad cset range")
					return CsetVal(c)
				}
				c.ranges[i] = runeRange{rune(lo), rune(hi)}
				prev = rune(hi)
			}
		}
		return CsetVal(c)
	case valBuiltin:
		if i := d.small(); i < len(d.builtins) {
			return BuiltinVal(d.builtins[i])
		}
		d.fail("builtin out of range")
	case valProc:
		if i := d.small(); i < len(d.procs) {
			return ProcVal(d.procs[i])
		}
		d.fail("procedure out of range")
	case valRecord:
		if i := d.small(); i < len(d.records) {
			return RecordTypeVal(d.records[i])
		}
		d.fail("record type out of range")
	default:
		d.fail("bad value tag %d", t)
	}
	return NullValue
}

// Validate checks that the indices in p are in range and that each
// instruction's operands are of the kinds the VM assumes, so that a bad
// program is reported rather than crashing the VM.
func Validate(p *Program) error {
	ng := len(p.Globals)
	if len(p.GlobalNames) != ng {
		return errors.New("global names do not match globals")
	}
	if p.Main != -1 {
		if p.Main < 0 || p.Main >= ng {
			return fmt.Errorf("main %d out of range", p.Main)
		}
		if _, ok := p.Globals[p.Main].AsProc(); !ok {
			return errors.New("main is not a procedure")
		}
	}
	if p.Invocable != nil && len(p.Invocable) != ng {
		return errors.New("invocable does not match globals")
	}
	seen := map[*Proc]bool{}
	for _, v := range p.Globals {
		pr, ok := v.AsProc()
		if !ok || seen[pr] {
			continue
		}
		seen[pr] = true
		if err := validateProc(p, pr); err != nil {
			return fmt.Errorf("procedure %s: %v", pr.Name, err)
		}
	}
	return nil
}

func validateProc(p *Program, pr *Proc) error {
	if pr.NParams > pr.NLocals || len(pr.LocalNames) > pr.NLocals {
		return errors.New("bad locals")
	}
	if pr.Variadic && pr.NParams == 0 {
		return errors.New("variadic with no parameters")
	}
	for _, s := range pr.Statics {
		if s >= len(p.Globals) {
			return fmt.Errorf("static %d out of range", s)
		}
	}
	if len(pr.Code) == 0 {
		return errors.New("no code")
	}
	inRange := func(o Operand) bool {
		i := int(o.Index)
		switch o.Kind {
		case NoOperand:
			return i == 0
		case TempOp:
			return i < pr.NTemps
		case LocalOp:
			return i < pr.NLocals
		case GlobalOp:
			return i < len(p.Globals)
		case ConstOp:
			return i < len(p.Consts)
		}
		return false
	}
	for pc := range pr.Code {
		in := &pr.Code[pc]
		bad := func(what string) error { return fmt.Errorf("instruction %d (%s): bad %s", pc, in.Op, what) }
		if in.Op >= numOps {
			return fmt.Errorf("instruction %d: bad opcode %d", pc, in.Op)
		}
		if !inRange(in.Dst) {
			return bad("destination")
		}
		if !inRange(in.X) {
			return bad("operand")
		}
		for _, a := range in.Args {
			if !inRange(a) || a.Kind == NoOperand {
				return bad("argument")
			}
		}
		if in.L1 < 0 || int(in.L1) >= len(pr.Code) || in.L2 < 0 || int(in.L2) >= len(pr.Code) {
			return bad("label")
		}
		if in.File < 0 || int(in.File) > len(pr.Files) {
			return bad("file")
		}
		if in.H < 0 || int(in.H) >= max(pr.NTemps, 1) {
			return bad("handle")
		}
		// The kinds the VM relies on.
		switch in.Op {
		case OpSetLabel, OpMove, OpMoveVar, OpOper, OpCall, OpResume, OpCreate, OpActivate, OpScanEnter:
			if in.Dst.Kind != TempOp {
				return bad("destination")
			}
		case OpAssign:
			if in.Dst.Kind == NoOperand {
				return bad("destination")
			}
		}
		switch in.Op {
		case OpIGoto, OpScanSwap:
			if in.X.Kind != TempOp {
				return bad("operand")
			}
		case OpOnce:
			if in.X.Kind != GlobalOp {
				return bad("operand")
			}
		case OpMove, OpMoveVar, OpAssign, OpCall, OpReturn, OpSuspend, OpIsNull, OpActivate, OpCoRet, OpScanEnter:
			if in.X.Kind == NoOperand {
				return bad("operand")
			}
		}
		switch in.Op {
		case OpOper:
			if in.Fn == nil {
				return bad("function")
			}
		case OpCall, OpResume:
			if pr.NTemps == 0 {
				return bad("handle")
			}
		}
	}
	return nil
}
