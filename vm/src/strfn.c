/*
 * String functions, as iconx's fstr.r: on bytes. gcon's UTF-8 helpers
 * (utf8valid, uchar) read or write UTF-8 where asked.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)

static void padargs(Value *a, int n, String *s1, int64_t *w, String *s2) {
	*s1 = muststr(ARG(0));
	*w = 1;
	if (!ISNULL(ARG(1)))
		*w = mustint(a[1]);
	if (*w < 0)
		runerrv(205, intval(*w));
	static const String blank = {" ", 1};
	*s2 = blank;
	if (!ISNULL(ARG(2))) {
		String s = muststr(a[2]);
		if (s.n > 0)
			*s2 = s;
	}
}

/* pad lays out a result of w bytes, as iconx's left, right and center
 * do: s1 (l bytes) at position start, and in the other positions bytes
 * of the pad s2, counted in copies of s2 from the left end before
 * position h and from the right end after it. */
static String pad(String s1, int64_t l, String s2, int64_t w, int64_t start, int64_t h) {
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
	return mkstr(r, w);
}

FN(f_left) {
	(void)b;
	String s1, s2;
	int64_t w;
	padargs(a, n, &s1, &w, &s2);
	if (w <= s1.n) {
		*out = strval(mkstr(s1.s, w));
		return true;
	}
	*out = strval(pad(s1, s1.n, s2, w, 0, 0));
	return true;
}

FN(f_right) {
	(void)b;
	String s1, s2;
	int64_t w;
	padargs(a, n, &s1, &w, &s2);
	int64_t l = s1.n;
	if (w <= l) {
		*out = strval(mkstr(s1.s + l - w, w));
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
	int64_t l = s1.n;
	if (w <= l) {
		*out = strval(mkstr(s1.s + ((l - w + 1) >> 1), w));
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
	if (s.n == 0 || k == 0) {
		*out = strval(mkstr("", 0));
		return true;
	}
	char *r = gcraw((size_t)(s.n * k));
	for (int64_t i = 0; i < k; i++)
		memcpy(r + i * s.n, s.s, (size_t)s.n);
	*out = strval(mkstr(r, s.n * k));
	return true;
}

FN(f_reverse) {
	(void)b;
	String s = muststr(ARG(0));
	char *r = gcraw((size_t)s.n + 1);
	for (int64_t i = 0; i < s.n; i++)
		r[i] = s.s[s.n - 1 - i];
	*out = strval(mkstr(r, s.n));
	return true;
}

static Cset *trimspace;

FN(f_trim) {
	(void)b;
	String s = muststr(ARG(0));
	Cset *c = ISNULL(ARG(1)) ? trimspace : mustcset(a[1]);
	int64_t end = s.n;
	while (end > 0 && cset_has(c, (uint8_t)s.s[end - 1]))
		end--;
	*out = strval(slice(s, 1, end + 1));
	return true;
}

/* map's translation table: the last one made is kept, as iconx does. */
typedef struct MapTab {
	Gen g; /* only its mark function: a map table is traced as a generator is */
	String s2, s3;
	uint8_t tab[256];
} MapTab;

static void mapmark(Gen *g) {
	MapTab *t = (MapTab *)g;
	markptr(t->s2.s);
	markptr(t->s3.s);
}

static MapTab *newmaptab(String s2, String s3) {
	if (s2.n != s3.n)
		runerr(208);
	MapTab *t = gcnew(sizeof *t, T_GEN);
	t->g.mark = mapmark;
	t->s2 = s2, t->s3 = s3;
	for (int i = 0; i < 256; i++)
		t->tab[i] = (uint8_t)i;
	for (int64_t i = 0; i < s2.n; i++)
		t->tab[(uint8_t)s2.s[i]] = (uint8_t)s3.s[i];
	return t;
}

static bool samestr(String a, String b) {
	return a.n == b.n && (a.s == b.s || memcmp(a.s, b.s, (size_t)a.n) == 0);
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
	char *r = gcraw((size_t)s1.n + 1);
	for (int64_t i = 0; i < s1.n; i++)
		r[i] = (char)t->tab[(uint8_t)s1.s[i]];
	*out = strval(mkstr(r, s1.n));
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
		char ch = s.s[k++];
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
			sbputc(&b, ch);
			if ((uint8_t)ch >= ' ' && (uint8_t)ch < 0x7F) /* isprint, as iconx */
				col++;
		}
	}
	return sbstring(&b);
}

FN(f_detab) { (void)b; *out = strval(tabs(a, n, false)); return true; }
FN(f_entab) { (void)b; *out = strval(tabs(a, n, true)); return true; }

FN(f_char) {
	(void)b;
	int64_t i = mustint(ARG(0));
	if (i < 0 || i > 255)
		runerrv(205, intval(i));
	char c = (char)i;
	*out = strval(strcopy(&c, 1));
	return true;
}

FN(f_ord) {
	(void)b;
	String s = muststr(ARG(0));
	if (s.n != 1)
		runerrv(205, strval(s));
	*out = intval((uint8_t)s.s[0]);
	return true;
}

/* ---- UTF-8 ---- */

FN(f_utf8valid) {
	(void)b;
	String s = muststr(ARG(0));
	*out = strval(s);
	for (int64_t i = 0; i < s.n;) {
		if ((uint8_t)s.s[i] < 0x80) {
			i++;
			continue;
		}
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
	*out = strval(strcopy((char *)e, k));
	return true;
}

static Builtin fns[] = {
	{.name = "left", .fn = f_left}, {.name = "right", .fn = f_right},
	{.name = "center", .fn = f_center}, {.name = "repl", .fn = f_repl},
	{.name = "reverse", .fn = f_reverse}, {.name = "trim", .fn = f_trim},
	{.name = "map", .fn = f_map}, {.name = "detab", .fn = f_detab},
	{.name = "entab", .fn = f_entab},
	{.name = "utf8valid", .fn = f_utf8valid}, {.name = "uchar", .fn = f_uchar},
	{.name = "char", .fn = f_char}, {.name = "ord", .fn = f_ord},
};

void strfn_init(void) {
	trimspace = csetfrom(" ", 1);
	gcroot((void **)&trimspace);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}
