/*
 * String functions (gcon's strfuncs.go and builtins.go's Unicode
 * functions), following iconx's fstr.r, in characters of the strings'
 * units.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)

/* Chars gives a string's characters by index. */
typedef struct Chars {
	const char *s;
	int64_t *offs; /* each character's offset, then the length; NULL if each is a byte */
	int64_t n;
} Chars;

static bool onebyte(String s) { return s.unit == BytesUnit || isascii_(&s); }

static Chars charsin(String s) {
	Chars c = {s.s, NULL, s.n};
	if (onebyte(s))
		return c;
	c.offs = gcraw((size_t)(s.n + 1) * sizeof(int64_t));
	int64_t k = 0;
	for (int64_t i = 0; i < s.n; i += width(s, i))
		c.offs[k++] = i;
	c.offs[k] = s.n;
	c.n = k;
	return c;
}

static int64_t coff(Chars c, int64_t i) { return c.offs ? c.offs[i] : i; }

static int joinunit(String *ss, int k) {
	int u = TextUnit;
	for (int i = 0; i < k; i++)
		if (!isascii_(&ss[i])) {
			if (ss[i].unit == TextUnit)
				return TextUnit;
			u = BytesUnit;
		}
	return u;
}

static void padargs(Value *a, int n, String *s1, int64_t *w, String *s2) {
	*s1 = muststr(ARG(0));
	*w = 1;
	if (!ISNULL(ARG(1)))
		*w = mustint(a[1]);
	if (*w < 0)
		runerrv(205, intval(*w));
	static const String blank = {" ", 1, TextUnit, 1};
	*s2 = blank;
	if (!ISNULL(ARG(2))) {
		String s = muststr(a[2]);
		if (s.n > 0)
			*s2 = s;
	}
}

static void padchar(SB *b, Chars c, int64_t i) {
	sbadd(b, c.s + coff(c, i), (size_t)(coff(c, i + 1) - coff(c, i)));
}

/* pad lays out a result of w characters, as iconx's left, right and
 * center do: s1 (l characters) at position start, and in the other
 * positions characters of the pad s2, counted in copies of s2 from the
 * left end before position h and from the right end after it. */
static String pad(String s1, int64_t l, String s2, int64_t w, int64_t start, int64_t h) {
	String ss[2] = {s1, s2};
	int unit = joinunit(ss, 2);
	int8_t ascii = isascii_(&s1) && isascii_(&s2) ? 1 : -1;
	if (onebyte(s1) && onebyte(s2)) {
		/* Every character a byte: the result is w bytes, written directly. */
		int64_t m = s2.n;
		char *r = gcraw((size_t)w);
		for (int64_t p = 0; p < w; p++) {
			if (p == start && l > 0) {
				memcpy(r + p, s1.s, (size_t)l);
				p += l - 1;
			} else if (p < h)
				r[p] = s2.s[p % m];
			else
				r[p] = s2.s[m - 1 - (w - 1 - p) % m];
		}
		return mkstrf(r, w, unit, ascii);
	}
	Chars pd = charsin(s2);
	int64_t m = pd.n;
	SB r = {0};
	sbgrow(&r, (size_t)(s1.n + (w - l) * (s2.n / m + 1)));
	for (int64_t p = 0; p < w; p++) {
		if (p == start && l > 0) {
			sbadd(&r, s1.s, (size_t)s1.n);
			p += l - 1;
		} else if (p < h)
			padchar(&r, pd, p % m);
		else
			padchar(&r, pd, m - 1 - (w - 1 - p) % m);
	}
	return mkstrf(r.b, (int64_t)r.n, unit, ascii);
}

FN(f_left) {
	(void)b;
	String s1, s2;
	int64_t w;
	padargs(a, n, &s1, &w, &s2);
	Chars c1 = charsin(s1);
	int64_t l = c1.n;
	if (w <= l) {
		*out = strval(mkstr(s1.s, coff(c1, w), s1.unit));
		return true;
	}
	*out = strval(pad(s1, l, s2, w, 0, 0));
	return true;
}

FN(f_right) {
	(void)b;
	String s1, s2;
	int64_t w;
	padargs(a, n, &s1, &w, &s2);
	Chars c1 = charsin(s1);
	int64_t l = c1.n;
	if (w <= l) {
		int64_t o = coff(c1, l - w);
		*out = strval(mkstr(s1.s + o, s1.n - o, s1.unit));
		return true;
	}
	*out = strval(pad(s1, l, s2, w, w - l, w));
	return true;
}

FN(f_center) {
	(void)b;
	String s1, s2;
	int64_t w;
	padargs(a, n, &s1, &w, &s2);
	Chars c1 = charsin(s1);
	int64_t l = c1.n;
	if (w <= l) {
		int64_t i = (l - w + 1) >> 1;
		int64_t o = coff(c1, i);
		*out = strval(mkstr(s1.s + o, coff(c1, i + w) - o, s1.unit));
		return true;
	}
	int64_t h = w / 2, start = h - l / 2;
	if (w % 2 == 0 && l % 2 == 1)
		start--;
	*out = strval(pad(s1, l, s2, w, start, h));
	return true;
}

FN(f_repl) {
	(void)b;
	String s = muststr(ARG(0));
	int64_t k = mustint(ARG(1));
	if (k < 0 || (s.n > 0 && k > ((int64_t)1 << 31) / s.n))
		runerrv(205, intval(k));
	SB r = {0};
	for (int64_t i = 0; i < k; i++)
		sbadd(&r, s.s, (size_t)s.n);
	*out = strval(sbstring(&r, s.unit));
	return true;
}

FN(f_reverse) {
	(void)b;
	String s = muststr(ARG(0));
	char *r = gcraw((size_t)s.n + 1);
	if (onebyte(s)) {
		for (int64_t i = 0; i < s.n; i++)
			r[i] = s.s[s.n - 1 - i];
	} else {
		int64_t p = s.n;
		for (int64_t i = 0; i < s.n;) {
			int w = width(s, i);
			p -= w;
			memcpy(r + p, s.s + i, (size_t)w);
			i += w;
		}
	}
	*out = strval(mkstr(r, s.n, s.unit));
	return true;
}

static Cset *trimspace;

FN(f_trim) {
	(void)b;
	String s = muststr(ARG(0));
	Cset *c = ISNULL(ARG(1)) ? trimspace : mustcset(a[1]);
	int64_t end = s.n;
	int w;
	if (onebyte(s)) {
		while (end > 0 && matchat(c, s, end - 1, &w))
			end--;
	} else {
		Chars cs = charsin(s);
		int64_t k = cs.n;
		while (k > 0 && matchat(c, s, cs.offs[k - 1], &w))
			k--;
		end = cs.offs[k];
	}
	*out = strval(slice(s, 1, end + 1));
	return true;
}

/* map's translation table: the last one made is kept, as iconx does. */
typedef struct MapTab {
	Gen g; /* only its mark function: a map table is traced as a generator is */
	String s2, s3;
	bool bytes;
	uint8_t tab[256];
	Chars c2, c3; /* otherwise: characters of s2 map to those of s3 */
} MapTab;

static void mapmark(Gen *g) {
	MapTab *t = (MapTab *)g;
	markptr(t->s2.s);
	markptr(t->s3.s);
	markptr(t->c2.offs);
	markptr(t->c3.offs);
}

static MapTab *newmaptab(String s2, String s3) {
	Chars c2 = charsin(s2), c3 = charsin(s3);
	if (c2.n != c3.n)
		runerr(208);
	MapTab *t = gcnew(sizeof *t, T_GEN);
	t->g.mark = mapmark;
	t->s2 = s2, t->s3 = s3;
	if (!c2.offs && !c3.offs) {
		t->bytes = true;
		for (int i = 0; i < 256; i++)
			t->tab[i] = (uint8_t)i;
		for (int64_t i = 0; i < s2.n; i++)
			t->tab[(uint8_t)s2.s[i]] = (uint8_t)s3.s[i];
		return t;
	}
	t->c2 = c2, t->c3 = c3;
	return t;
}

static bool samestr(String a, String b) {
	return a.n == b.n && a.unit == b.unit && memcmp(a.s, b.s, (size_t)a.n) == 0;
}

FN(f_map) {
	(void)b;
	static String ucase, lcase;
	if (!ucase.s) {
		ucase = cset_string(CsetUcase);
		lcase = cset_string(CsetLcase);
	}
	String s1 = muststr(ARG(0)), s2 = ucase, s3 = lcase;
	if (!ISNULL(ARG(1)))
		s2 = muststr(a[1]);
	if (!ISNULL(ARG(2)))
		s3 = muststr(a[2]);
	MapTab *t = vm.maptab;
	if (!t || !samestr(t->s2, s2) || !samestr(t->s3, s3)) {
		t = newmaptab(s2, s3);
		vm.maptab = t;
	}
	String ss[2] = {s1, s3};
	if (t->bytes && onebyte(s1)) {
		char *r = gcraw((size_t)s1.n + 1);
		for (int64_t i = 0; i < s1.n; i++)
			r[i] = (char)t->tab[(uint8_t)s1.s[i]];
		*out = strval(mkstrf(r, s1.n, joinunit(ss, 2), 0));
		return true;
	}
	SB r = {0};
	{
		for (int64_t i = 0; i < s1.n;) {
			int w = width(s1, i);
			const char *c = s1.s + i;
			if (t->bytes && w == 1)
				sbputc(&r, t->tab[(uint8_t)c[0]]);
			else if (t->bytes)
				sbadd(&r, c, (size_t)w);
			else {
				/* the last occurrence in s2 takes precedence */
				int64_t found = -1;
				for (int64_t k = t->c2.n - 1; k >= 0; k--) {
					int64_t o = coff(t->c2, k), e = coff(t->c2, k + 1);
					if (e - o == w && memcmp(t->c2.s + o, c, (size_t)w) == 0) {
						found = k;
						break;
					}
				}
				if (found >= 0)
					padchar(&r, t->c3, found);
				else
					sbadd(&r, c, (size_t)w);
			}
			i += w;
		}
	}
	*out = strval(sbstring(&r, joinunit(ss, 2)));
	return true;
}

/* Tab stops, as iconx's nxttab. */
typedef struct Tabs {
	int64_t *stops;
	int nstops, next;
	int64_t last, interval;
} Tabs;

static void tabreset(Tabs *t) { t->next = 0, t->last = 1; }

static int64_t tabafter(Tabs *t, int64_t col) {
	while (t->next < t->nstops && col >= t->stops[t->next]) {
		t->next++;
		if (t->next == t->nstops)
			t->interval = t->stops[t->next - 1] - t->last;
		else
			t->last = t->stops[t->next - 1];
	}
	if (t->next >= t->nstops)
		return col + t->interval - (col - t->last) % t->interval;
	return t->stops[t->next];
}

static bool printable(const char *c, int w, int unit) {
	if (w == 1)
		return (uint8_t)c[0] >= ' ' && (uint8_t)c[0] < 0x7F;
	int ww;
	int32_t r = utf8decode((const uint8_t *)c, (size_t)w, &ww);
	return unit == TextUnit && r != RuneError && isprint_rune(r);
}

static String tabs(Value *a, int n, bool entab) {
	String s = muststr(ARG(0));
	Tabs t = {0};
	t.stops = gcnew((size_t)(n + 1) * sizeof(int64_t), T_RAW);
	for (int k = 1; k < n; k++) {
		int64_t i = mustint(a[k]);
		if ((k == 1 && i <= 1) || (k > 1 && i <= t.stops[k - 2]))
			runerrv(210, a[k]);
		t.stops[t.nstops++] = i;
	}
	t.last = 1, t.interval = 8;
	SB b = {0};
	int64_t col = 1;
	for (int64_t k = 0; k < s.n;) {
		int w = width(s, k);
		const char *c = s.s + k;
		k += w;
		char ch = w == 1 ? c[0] : 0;
		if (ch == '\b') {
			sbputc(&b, ch);
			col--;
			tabreset(&t);
		} else if (ch == '\n' || ch == '\r') {
			sbputc(&b, ch);
			col = 1;
			tabreset(&t);
		} else if (ch == '\t' && !entab) {
			int64_t target = tabafter(&t, col);
			for (; col < target; col++)
				sbputc(&b, ' ');
		} else if (ch == '\t') {
			sbputc(&b, ch);
			col = tabafter(&t, col);
		} else if (ch == ' ' && entab) {
			int64_t target = col + 1;
			while (k < s.n && s.s[k] == ' ')
				target++, k++;
			if (target - col <= 1) {
				sbputc(&b, ' ');
				col = target;
				continue;
			}
			int64_t nt = tabafter(&t, col);
			if (nt == col + 1) {
				int64_t nt1 = tabafter(&t, nt);
				if (nt1 > target) {
					sbputc(&b, ' ');
					col++;
					nt = nt1;
				}
			}
			while (nt <= target) {
				sbputc(&b, '\t');
				col = nt;
				nt = tabafter(&t, nt);
			}
			for (; col < target; col++)
				sbputc(&b, ' ');
			col = target;
		} else {
			sbadd(&b, c, (size_t)w);
			if (printable(c, w, s.unit))
				col++;
		}
	}
	return sbstring(&b, s.unit);
}

FN(f_detab) { (void)b; *out = strval(tabs(a, n, false)); return true; }
FN(f_entab) { (void)b; *out = strval(tabs(a, n, true)); return true; }

/* ---- Unicode functions ---- */

FN(f_runes) {
	(void)b;
	if (VKIND(ARG(0)) == KCset) {
		*out = a[0];
		return true;
	}
	String s = muststr(ARG(0));
	s.unit = TextUnit;
	*out = strval(s);
	return true;
}

FN(f_bytes) {
	(void)b;
	if (VKIND(ARG(0)) == KCset) {
		*out = ptrval(KCset, cset_asbytes(a[0].w.p));
		return true;
	}
	String s = muststr(ARG(0));
	s.unit = BytesUnit;
	*out = strval(s);
	return true;
}

FN(f_len) {
	(void)b;
	*out = intval(runes(muststr(ARG(0))));
	return true;
}

FN(f_utf8valid) {
	(void)b;
	String s = muststr(ARG(0));
	*out = strval(s);
	for (int64_t i = 0; i < s.n;) {
		int w;
		int32_t r = utf8decode((const uint8_t *)s.s + i, (size_t)(s.n - i), &w);
		if (r == RuneError && w == 1)
			return false;
		i += w;
	}
	return true;
}

FN(f_uchar) {
	(void)b;
	int64_t i = mustint(ARG(0));
	int32_t r = RuneError;
	if (i >= 0 && i <= MaxRune)
		r = (int32_t)i;
	uint8_t e[4];
	int k = utf8encode(e, r);
	*out = strval(strcopy((char *)e, k, TextUnit));
	return true;
}

FN(f_char) {
	(void)b;
	int64_t i = mustint(ARG(0));
	if (i < 0 || i > 255)
		runerrv(205, intval(i));
	char c = (char)i;
	*out = strval(strcopy(&c, 1, i < 0x80 ? TextUnit : BytesUnit));
	return true;
}

FN(f_ord) {
	(void)b;
	String s = muststr(ARG(0));
	if (runes(s) != 1)
		runerrv(205, strval(s));
	if (s.unit == BytesUnit || isascii_(&s)) {
		*out = intval((uint8_t)s.s[0]);
		return true;
	}
	int w;
	*out = intval(utf8decode((const uint8_t *)s.s, (size_t)s.n, &w));
	return true;
}

static Builtin fns[] = {
	{.name = "left", .fn = f_left}, {.name = "right", .fn = f_right},
	{.name = "center", .fn = f_center}, {.name = "repl", .fn = f_repl},
	{.name = "reverse", .fn = f_reverse}, {.name = "trim", .fn = f_trim},
	{.name = "map", .fn = f_map}, {.name = "detab", .fn = f_detab},
	{.name = "entab", .fn = f_entab}, {.name = "runes", .fn = f_runes},
	{.name = "bytes", .fn = f_bytes}, {.name = "len", .fn = f_len},
	{.name = "utf8valid", .fn = f_utf8valid}, {.name = "uchar", .fn = f_uchar},
	{.name = "char", .fn = f_char}, {.name = "ord", .fn = f_ord},
};

void strfn_init(void) {
	trimspace = csetfrom(" ", 1, TextUnit);
	gcroot((void **)&trimspace);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}
