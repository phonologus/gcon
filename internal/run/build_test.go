package run

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/phonologus/gcon/internal/corpus"
	"github.com/phonologus/gcon/internal/vm"
)

// roundTrip checks that p survives encoding: it validates, the decoded
// program lists the same, its builtins are the very same values, its
// reals have the same bits, and encoding it again gives the same bytes.
func roundTrip(t *testing.T, name string, p *vm.Program) {
	t.Helper()
	if err := vm.Validate(p); err != nil {
		t.Errorf("%s: compiled program does not validate: %v", name, err)
		return
	}
	enc, err := vm.Encode(p, BuildInfo())
	if err != nil {
		t.Errorf("%s: encode: %v", name, err)
		return
	}
	q, info, err := vm.Decode(enc)
	if err != nil {
		t.Errorf("%s: decode: %v", name, err)
		return
	}
	var a, b bytes.Buffer
	vm.Disasm(&a, p, info)
	vm.Disasm(&b, q, info)
	if a.String() != b.String() {
		t.Errorf("%s: decoded program differs\n--- compiled\n%s--- decoded\n%s", name, a.String(), b.String())
		return
	}
	same := func(what string, x, y vm.Value) {
		switch x := x.(type) {
		case *vm.Builtin:
			if x != y {
				t.Errorf("%s: %s: builtin %s not restored as the same value", name, what, x.Name)
			}
		case vm.Real:
			if math.Float64bits(float64(x)) != math.Float64bits(float64(y.(vm.Real))) {
				t.Errorf("%s: %s: real %v changed", name, what, x)
			}
		}
	}
	for i := range p.Consts {
		same("constant", p.Consts[i], q.Consts[i])
	}
	for i := range p.Globals {
		same("global", p.Globals[i], q.Globals[i])
		if pp, ok := p.Globals[i].(*vm.Proc); ok {
			qp := q.Globals[i].(*vm.Proc)
			for j := range pp.Code {
				if pp.Code[j].Fn != qp.Code[j].Fn {
					t.Errorf("%s: %s %d: builtin not restored as the same value", name, pp.Name, j)
				}
			}
		}
	}
	enc2, err := vm.Encode(q, BuildInfo())
	if err != nil || !bytes.Equal(enc, enc2) {
		t.Errorf("%s: encoding the decoded program differs (%v)", name, err)
	}
}

// TestRoundTrip encodes and decodes every test program.
func TestRoundTrip(t *testing.T) {
	files, _ := filepath.Glob("testdata/*.icn")
	more, _ := filepath.Glob("testdata/traceback/*.icn")
	files = append(files, more...)
	lib := []string{filepath.Join("testdata", "lib")}
	n := 0
	for _, f := range files {
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		p, errs := TranslateLinked(f, string(src), lib, lib)
		if len(errs) > 0 {
			continue // programs that test translation errors
		}
		roundTrip(t, f, p)
		n++
	}
	if n < 50 {
		t.Errorf("only %d programs translated", n)
	}
}

// TestRoundTripCorpus encodes and decodes every program of the reference
// tree that gcon translates, linking the IPL from source.
func TestRoundTripCorpus(t *testing.T) {
	if testing.Short() {
		t.Skip("short")
	}
	root := corpus.Require(t)
	lib := []string{filepath.Join(root, "ipl", "procs"), filepath.Join(root, "ipl", "gprocs")}
	incl := []string{filepath.Join(root, "ipl", "incl"), filepath.Join(root, "ipl", "gincl")}
	n := 0
	for _, f := range corpus.IconFiles(t) {
		src, err := os.ReadFile(f)
		if err != nil || !bytes.Contains(src, []byte("procedure main")) {
			continue
		}
		p, errs := TranslateLinked(f, string(src), lib, incl)
		if len(errs) > 0 || p.Main < 0 {
			continue
		}
		roundTrip(t, f, p)
		n++
	}
	t.Logf("%d programs round-tripped", n)
}

// sample returns a small built program, encoded.
func sample(t *testing.T) []byte {
	t.Helper()
	src := `record r(a, b)
procedure main(args)
   static s
   initial s := 'xyz'
   x := r(1.5, 2^80)
   every write(!args | x.a | "é" | -3)
   write(s)
end
`
	p, errs := TranslateLinked("sample.icn", src, []string{}, []string{})
	if len(errs) > 0 {
		t.Fatal(errs)
	}
	enc, err := vm.Encode(p, BuildInfo())
	if err != nil {
		t.Fatal(err)
	}
	return enc
}

// TestDamage checks that every truncation and every changed byte of a
// program is reported, not run and not a crash.
func TestDamage(t *testing.T) {
	enc := sample(t)
	if _, _, err := vm.Decode(enc); err != nil {
		t.Fatal(err)
	}
	for n := 0; n < len(enc); n++ {
		if _, _, err := vm.Decode(enc[:n]); err == nil {
			t.Errorf("truncated to %d bytes: no error", n)
		}
	}
	b := make([]byte, len(enc))
	for i := range enc {
		for _, x := range []byte{0x01, 0x80, 0xff} {
			copy(b, enc)
			b[i] ^= x
			if _, _, err := vm.Decode(b); err == nil {
				t.Errorf("byte %d ^ %#x: no error", i, x)
			}
		}
	}
}

// withSection inserts a section after the version, resealing the
// checksum.
func withSection(enc []byte, tag, body string) []byte {
	at := len(vm.Magic) + 1 // the version is one byte
	var s []byte
	s = append(s, tag...)
	s = binary.AppendUvarint(s, uint64(len(body)))
	s = append(s, body...)
	b := append(append(append([]byte(nil), enc[:at]...), s...), enc[at:]...)
	end := len(b) - 9 // "End ", length 4, CRC
	binary.LittleEndian.PutUint32(b[len(b)-4:], crc32.ChecksumIEEE(b[:end]))
	return b
}

func TestSections(t *testing.T) {
	enc := sample(t)
	if _, _, err := vm.Decode(withSection(enc, "zfut", "anything")); err != nil {
		t.Errorf("unknown optional section: %v", err)
	}
	_, _, err := vm.Decode(withSection(enc, "Zfut", "anything"))
	if err == nil || !strings.Contains(err.Error(), "newer gcon") {
		t.Errorf("unknown required section: got %v", err)
	}
	b := append([]byte(nil), enc...)
	b[len(vm.Magic)] = vm.FormatVersion + 1
	_, _, err = vm.Decode(b)
	if err == nil || !strings.Contains(err.Error(), "rebuild") {
		t.Errorf("newer version: got %v", err)
	}
	_, info, err := vm.Decode(enc)
	if err != nil || info["gcon"] == "" || !strings.Contains(info["predefined"], "_V9=1") {
		t.Errorf("build information: %v, %v", info, err)
	}
}

// TestBuiltFile checks the built file: its header, its mode, and that
// run.File recognises it.
func TestBuiltFile(t *testing.T) {
	t.Chdir(t.TempDir())
	if err := os.WriteFile("hi.icn", []byte("procedure main()\n write(&progname)\nend\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if errs := BuildFile("hi.icn", "hi", "/opt/it's/gcon", Config{LibPath: []string{}, IncludePath: []string{}}); errs != nil {
		t.Fatal(errs)
	}
	b, err := os.ReadFile("hi")
	if err != nil {
		t.Fatal(err)
	}
	if !IsBuilt(b) || !bytes.HasPrefix(b, []byte("#!/bin/sh\nGCBIN='/opt/it'\\''s/gcon'\n")) {
		t.Errorf("header:\n%s", b[:bytes.Index(b, []byte(marker))])
	}
	if st, _ := os.Stat("hi"); st.Mode().Perm()&0o100 == 0 {
		t.Errorf("not executable: %v", st.Mode())
	}
	var out bytes.Buffer
	if s := File("./hi", Config{Stdout: &out, Stderr: &out}); s != 0 || out.String() != "./hi\n" {
		t.Errorf("status %d, output %q", s, out.String())
	}
	if IsBuilt([]byte("#!/usr/bin/env gcon\n# " + marker + "procedure main()\nend\n")) {
		t.Error("source mentioning the marker taken as built")
	}
}
