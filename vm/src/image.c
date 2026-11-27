/*
 * Images of values, as gcon's (internal/vm: ops.go quote, cset.go
 * imageCset, real.go rtos, bigint.go bigImage, disasm.go constImage),
 * and Go's %q for messages that name a section.
 */
#include "gcon-vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const builtinkinds[NumBuiltinKinds] = {
	"op", "function", "asgn", "apply", "field", "unsupported"
};

void builtinref(Buf *b, const BuiltinRef *r) {
	if (r->kind == BuiltinApply) {
		bufputs(b, "apply");
		return;
	}
	if (r->kind < NumBuiltinKinds)
		bufputs(b, builtinkinds[r->kind]);
	else
		bufprintf(b, "kind%d", r->kind);
	bufputc(b, ' ');
	bufstr(b, r->name);
}

/* goquote writes s as Go's strconv.Quote does. */
void goquote(Buf *b, const char *s, size_t n) {
	const uint8_t *p = (const uint8_t *)s;
	bufputc(b, '"');
	for (size_t i = 0; i < n;) {
		int w;
		int32_t r = utf8decode(p + i, n - i, &w);
		if (w == 1 && r == RuneError) {
			bufprintf(b, "\\x%02x", p[i]);
			i++;
			continue;
		}
		if (r == '"' || r == '\\') {
			bufputc(b, '\\');
			bufputc(b, r);
		} else if (isprint_rune(r))
			bufadd(b, p + i, (size_t)w);
		else
			switch (r) {
			case '\a': bufputs(b, "\\a"); break;
			case '\b': bufputs(b, "\\b"); break;
			case '\f': bufputs(b, "\\f"); break;
			case '\n': bufputs(b, "\\n"); break;
			case '\r': bufputs(b, "\\r"); break;
			case '\t': bufputs(b, "\\t"); break;
			case '\v': bufputs(b, "\\v"); break;
			default:
				if (r < ' ' || r == 0x7F)
					bufprintf(b, "\\x%02x", r);
				else if (r < 0x10000)
					bufprintf(b, "\\u%04x", r);
				else
					bufprintf(b, "\\U%08x", r);
			}
		i += (size_t)w;
	}
	bufputc(b, '"');
}

/* quotebyte writes one byte of an Icon image, as iconx escapes it. */
static void quotebyte(Buf *b, uint8_t c, char q) {
	switch (c) {
	case '\b': bufputs(b, "\\b"); return;
	case '\t': bufputs(b, "\\t"); return;
	case '\n': bufputs(b, "\\n"); return;
	case '\v': bufputs(b, "\\v"); return;
	case '\f': bufputs(b, "\\f"); return;
	case '\r': bufputs(b, "\\r"); return;
	case 033: bufputs(b, "\\e"); return;
	case 0177: bufputs(b, "\\d"); return;
	}
	if (c == (uint8_t)q || c == '\\') {
		bufputc(b, '\\');
		bufputc(b, c);
	} else if (c < ' ' || c >= 0x80)
		bufprintf(b, "\\x%02x", c);
	else
		bufputc(b, c);
}

/* quote writes s between q quotes: bytes as iconx escapes them, except
 * that, unless bytes is set, a valid UTF-8 sequence appears as itself if
 * its code point is printable and as a \u or \U escape if not, which a
 * literal reads back as the same bytes. */
void quote(Buf *b, const char *s, size_t n, bool bytes, char q) {
	const uint8_t *p = (const uint8_t *)s;
	bufputc(b, q);
	for (size_t i = 0; i < n;) {
		uint8_t c = p[i];
		if (c < RuneSelf || bytes) {
			quotebyte(b, c, q);
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode(p + i, n - i, &w);
		if (r == RuneError && w == 1)
			quotebyte(b, c, q);
		else if (isprint_rune(r))
			bufadd(b, p + i, (size_t)w);
		else if (r < 0x10000)
			bufprintf(b, "\\u%04x", r);
		else
			bufprintf(b, "\\U%06x", r);
		i += (size_t)w;
	}
	bufputc(b, q);
}

static bool has(const Cset *c, int x) { return c->bits[x >> 6] >> (x & 63) & 1; }

static bool bitsare(const Cset *c, uint64_t b0, uint64_t b1, uint64_t b2, uint64_t b3) {
	return c->bits[0] == b0 && c->bits[1] == b1 && c->bits[2] == b2 && c->bits[3] == b3;
}

/* csetname returns the keyword naming a cset with c's members, or NULL,
 * as iconx's csname. */
static const char *csetname(const Cset *c) {
	const uint64_t all = ~(uint64_t)0;
	const uint64_t digits = (uint64_t)0x3FF << 48;
	const uint64_t ucase = (uint64_t)0x3FFFFFF << 1, lcase = (uint64_t)0x3FFFFFF << 33;
	if (bitsare(c, 0, ucase | lcase, 0, 0))
		return "&letters";
	if (bitsare(c, 0, lcase, 0, 0))
		return "&lcase";
	if (bitsare(c, 0, ucase, 0, 0))
		return "&ucase";
	if (bitsare(c, digits, 0, 0, 0))
		return "&digits";
	if (bitsare(c, all, all, 0, 0))
		return "&ascii";
	if (bitsare(c, all, all, all, all))
		return "&cset";
	return NULL;
}

/* imagecset writes image(c) as iconx: a predefined cset's name, else
 * every member, in order. */
void imagecset(Buf *b, const Cset *c) {
	const char *name = csetname(c);
	if (name) {
		bufputs(b, name);
		return;
	}
	char t[256];
	size_t n = 0;
	for (int x = 0; x < 256; x++)
		if (has(c, x))
			t[n++] = (char)x;
	quote(b, t, n, true, '\'');
}

/* rtos writes a real as iconx's rtos: C's "%.10g", with ".0" added when
 * there is neither a decimal point nor an exponent. */
void rtos(Buf *b, double f) {
	if (isinf(f)) {
		bufputs(b, f > 0 ? "inf.0" : "-inf.0");
		return;
	}
	if (isnan(f)) {
		bufputs(b, "nan.0");
		return;
	}
	char s[64];
	snprintf(s, sizeof s, "%.10g", f + 0.0); /* + 0 avoids "-0" */
	bufputs(b, s);
	if (strpbrk(s, ".e") == NULL)
		bufputs(b, ".0");
}

/* maxDigits: a large integer with an estimated this many digits or more
 * has the image integer(~10^n), as iconx's. */
enum { MaxDigits = 30 };

void bigimage(Buf *b, const Big *x) {
	int slen = x->n - 1; /* index of the most significant digit */
	int64_t dlen = (int64_t)((double)(slen * 32) * 0.3010299956639812 +
				 log((double)x->d[slen]) * 0.4342944819032518 + 0.5);
	if (dlen >= MaxDigits) {
		bufprintf(b, "integer(~10^%lld)", (long long)dlen);
		return;
	}
	/* Divide by 10^9 repeatedly, least significant group first. */
	uint32_t *d = xmalloc((size_t)x->n * sizeof *d);
	memcpy(d, x->d, (size_t)x->n * sizeof *d);
	uint32_t groups[8];
	int ng = 0, n = x->n;
	while (n > 0) {
		uint64_t rem = 0;
		for (int i = n - 1; i >= 0; i--) {
			uint64_t cur = rem << 32 | d[i];
			d[i] = (uint32_t)(cur / 1000000000);
			rem = cur % 1000000000;
		}
		groups[ng++] = (uint32_t)rem;
		while (n > 0 && d[n - 1] == 0)
			n--;
	}
	free(d);
	if (x->neg)
		bufputc(b, '-');
	bufprintf(b, "%u", groups[ng - 1]);
	for (int i = ng - 2; i >= 0; i--)
		bufprintf(b, "%09u", groups[i]);
}

/* constimage writes the listing of a constant or a global's initial
 * value, as gcon's constImage. */
void constimage(Buf *b, const Program *p, Value v) {
	switch (VKIND(v)) {
	case KNull:
		bufputs(b, "&null");
		break;
	case KInt:
		bufprintf(b, "%lld", (long long)v.w.i);
		break;
	case KBig:
		bigimage(b, v.w.p);
		break;
	case KReal:
		rtos(b, v.w.r);
		break;
	case KString:
		quote(b, v.w.s, VN(v), false, '"');
		break;
	case KCset:
		imagecset(b, v.w.p);
		break;
	case KProc: {
		const Proc *pr = v.w.p;
		bufputs(b, "procedure ");
		bufstr(b, pr->name);
		break;
	}
	case KRecordType: {
		const RecordType *r = v.w.p;
		bufputs(b, "record ");
		bufstr(b, r->name);
		bufputc(b, '(');
		for (int i = 0; i < r->nfields; i++) {
			if (i > 0)
				bufputs(b, ", ");
			bufstr(b, r->fields[i]);
		}
		bufputc(b, ')');
		break;
	}
	case KBuiltin:
		builtinref(b, &p->builtins[VN(v)]);
		break;
	default:
		bufprintf(b, "? kind %d", VKIND(v));
	}
}
