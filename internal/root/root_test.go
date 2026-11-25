package root

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFind(t *testing.T) {
	roots := map[string]bool{
		"/opt/gcon":     true,
		"/usr/share/gc": true,
		"/home/u/go/pkg/mod/github.com/phonologus/gcon@v1.2.3":          true,
		"/cache/github.com/phonologus/gcon@v1.2.3":                      true,
		"/cache/github.com/phonologus/gcon@v1.0.0-!r!c1":                true,
		"/home/u/go/pkg/mod/github.com/phonologus/gcon@v0.0.0-2026-abc": false,
	}
	mod := [2]string{ModulePath, "v1.2.3"}
	tests := []struct {
		name string
		vars map[string]string
		exe  string
		def  string
		mod  [2]string
		want Root
	}{
		{"env wins, as it is", map[string]string{"GCONROOT": "/anywhere"}, "/opt/gcon/bin/gcon", "/usr/share/gc", mod,
			Root{"/anywhere", FromEnv}},
		{"beside the executable", nil, "/opt/gcon/bin/gcon", "/usr/share/gc", mod,
			Root{"/opt/gcon", FromExecutable}},
		{"default when the executable has no root", nil, "/usr/bin/gcon", "/usr/share/gc", mod,
			Root{"/usr/share/gc", FromDefault}},
		{"module cache under HOME", map[string]string{"HOME": "/home/u"}, "/home/u/go/bin/gcon", "", mod,
			Root{"/home/u/go/pkg/mod/github.com/phonologus/gcon@v1.2.3", FromModCache}},
		{"GOMODCACHE before GOPATH", map[string]string{"GOMODCACHE": "/cache", "GOPATH": "/elsewhere", "HOME": "/home/u"}, "", "", mod,
			Root{"/cache/github.com/phonologus/gcon@v1.2.3", FromModCache}},
		{"escaped version", map[string]string{"GOMODCACHE": "/cache"}, "", "", [2]string{ModulePath, "v1.0.0-RC1"},
			Root{"/cache/github.com/phonologus/gcon@v1.0.0-!r!c1", FromModCache}},
		{"no module cache for a devel build", map[string]string{"HOME": "/home/u"}, "/x/bin/gcon", "", [2]string{ModulePath, "(devel)"},
			Root{"", NotFound}},
		{"no module cache for another main module", map[string]string{"HOME": "/home/u"}, "", "", [2]string{"example.com/other", "v1.2.3"},
			Root{"", NotFound}},
		{"module cache without the library", map[string]string{"HOME": "/home/u"}, "", "", [2]string{ModulePath, "v0.0.0-2026-abc"},
			Root{"", NotFound}},
	}
	for _, tt := range tests {
		got := find(env{
			getenv:   func(k string) string { return tt.vars[k] },
			exe:      tt.exe,
			def:      tt.def,
			buildMod: tt.mod,
			isRoot:   func(d string) bool { return roots[filepath.ToSlash(d)] },
		})
		got.Dir = filepath.ToSlash(got.Dir)
		if got != tt.want {
			t.Errorf("%s: got %+v, want %+v", tt.name, got, tt.want)
		}
	}
}

func TestModCache(t *testing.T) {
	for _, tt := range []struct {
		vars map[string]string
		want string
	}{
		{map[string]string{"GOMODCACHE": "/c", "GOPATH": "/g"}, "/c"},
		{map[string]string{"GOPATH": "/g1" + string(filepath.ListSeparator) + "/g2"}, "/g1/pkg/mod"},
		{map[string]string{"HOME": "/h"}, "/h/go/pkg/mod"},
		{nil, ""},
	} {
		if got := filepath.ToSlash(modCache(func(k string) string { return tt.vars[k] })); got != tt.want {
			t.Errorf("modCache(%v) = %q, want %q", tt.vars, got, tt.want)
		}
	}
}

// TestRepositoryIsRoot checks that the repository is a root, and that
// its copy of the library is exactly what lib/ipl/UPSTREAM records:
// every file listed, unmodified, and nothing else.
func TestRepositoryIsRoot(t *testing.T) {
	repo := filepath.Join("..", "..")
	if !IsRoot(repo) {
		t.Fatal("the repository is not a gcon root")
	}
	ipl := filepath.Join(repo, "lib", "ipl")
	f, err := os.Open(filepath.Join(ipl, "UPSTREAM"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	listed := map[string]string{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if rest, ok := strings.CutPrefix(sc.Text(), "sha256 "); ok {
			sum, name, _ := strings.Cut(rest, "  ")
			listed[name] = sum
		}
	}
	if len(listed) < 390 {
		t.Fatalf("UPSTREAM lists %d files", len(listed))
	}
	seen := 0
	for _, d := range append(append([]string(nil), LinkDirs...), IncludeDirs...) {
		dir := filepath.Join(repo, filepath.FromSlash(d))
		err := filepath.WalkDir(dir, func(p string, e fs.DirEntry, err error) error {
			if err != nil || e.IsDir() {
				return err
			}
			rel, _ := filepath.Rel(ipl, p)
			rel = filepath.ToSlash(rel)
			want, ok := listed[rel]
			if !ok {
				t.Errorf("%s is not in UPSTREAM", rel)
				return nil
			}
			b, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			sum := sha256.Sum256(b)
			if hex.EncodeToString(sum[:]) != want {
				t.Errorf("%s differs from UPSTREAM (edit a copy on IPATH instead)", rel)
			}
			seen++
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	if seen != len(listed) {
		t.Errorf("%d files present, UPSTREAM lists %d", seen, len(listed))
	}
}
