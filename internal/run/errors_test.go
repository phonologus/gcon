package run

import (
	"strings"
	"testing"
)

// TestTranslationErrors checks error messages that icont gives
// (expected messages taken from icont's output).
func TestTranslationErrors(t *testing.T) {
	tests := []struct{ src, want string }{
		{"procedure main()\n  create (1 | return 2)\nend\n", "File t.icn; Line 2 # invalid context for return or fail"},
		{"procedure main()\n  create (1 | fail)\nend\n", "File t.icn; Line 2 # invalid context for return or fail"},
		{"procedure main()\n  create (suspend 5)\nend\n", "File t.icn; Line 2 # invalid context for suspend"},
	}
	for _, tt := range tests {
		_, errs := Translate("t.icn", tt.src)
		var got []string
		for _, e := range errs {
			got = append(got, e.Error())
		}
		if strings.Join(got, "\n") != tt.want {
			t.Errorf("%q:\ngot  %q\nwant %q", tt.src, got, tt.want)
		}
	}
}
