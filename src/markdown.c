/*
 * The documentation's Markdown (markdown.h): a line-at-a-time reader of
 * the subset, without recursion. Inline markup is read when a block's
 * text is complete.
 */
#include "markdown.h"

#include <stdio.h>
#include <string.h>

typedef struct Level {
	int mi, ci; /* the marker's indentation, the item text's */
	bool ordered;
} Level;

typedef struct Parser {
	MdDoc *d;
	Level lists[2];
	int nlists;
	Buf text;      /* the open text block's text so far */
	int textblock; /* its index in d->blocks, or -1 */
	StrMap anchors;
} Parser;

static void err(Parser *p, int line, const char *msg) {
	Buf b = {0};
	bufprintf(&b, "line %d: %s", line, msg);
	VPUSH(p->d->errs, b.b);
}

static bool ispunct_(int c) { return c > ' ' && c < 0x7f && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); }

/* ------------------------------------------------------------------ */
/* Inline */

static void span(MdSpans *s, MdSpanKind k, bool end, const char *t, size_t n) {
	MdSpan sp = {k, end, t ? xstrndup(t, n) : NULL};
	VPUSH(*s, sp);
}

/* flushtext adds the text gathered in b as a span. */
static void flushtext(MdSpans *s, Buf *b) {
	if (b->n) {
		span(s, MS_TEXT, false, b->b, b->n);
		b->n = 0;
		b->b[0] = 0;
	}
}

/* inlines reads the inline markup of s (n bytes) into spans. */
static MdSpans inlines(Parser *p, int line, const char *s, size_t n) {
	MdSpans out = {0};
	Buf t = {0};
	bool em = false, strong = false;
	int order[2], nopen = 0; /* the open emphases, inner last: 1 em, 2 strong */
	size_t linkend = 0;      /* the "]" ending the open link, or 0 */
	const char *target = NULL;
	size_t targetn = 0;
	for (size_t i = 0; i < n;) {
		char c = s[i];
		if (linkend && i == linkend) {
			flushtext(&out, &t);
			span(&out, MS_LINK, true, NULL, 0);
			i = (size_t)(target - s) + targetn + 1; /* past ")" */
			linkend = 0;
			continue;
		}
		if (c == '\\' && i + 1 < n && ispunct_((unsigned char)s[i + 1])) {
			bufputc(&t, s[i + 1]);
			i += 2;
			continue;
		}
		if (c == '`') {
			size_t k = 0;
			while (i + k < n && s[i + k] == '`')
				k++;
			size_t j = i + k, close = 0;
			while (j < n) {
				if (s[j] == '`') {
					size_t m = 0;
					while (j + m < n && s[j + m] == '`')
						m++;
					if (m == k) {
						close = j;
						break;
					}
					j += m;
				} else
					j++;
			}
			if (!close) {
				err(p, line, "a ` that is never closed");
				bufadd(&t, s + i, k);
				i += k;
				continue;
			}
			const char *c0 = s + i + k;
			size_t cn = close - (i + k);
			bool allsp = true;
			for (size_t q = 0; q < cn; q++)
				allsp = allsp && c0[q] == ' ';
			if (cn >= 2 && c0[0] == ' ' && c0[cn - 1] == ' ' && !allsp)
				c0++, cn -= 2;
			flushtext(&out, &t);
			span(&out, MS_CODE, false, c0, cn);
			i = close + k;
			continue;
		}
		if (c == '*') {
			int which = (i + 1 < n && s[i + 1] == '*') ? 2 : 1;
			bool *open = which == 2 ? &strong : &em;
			flushtext(&out, &t);
			if (*open) {
				if (nopen == 0 || order[nopen - 1] != which)
					err(p, line, "emphasis closed out of order");
				else
					nopen--;
				*open = false;
				span(&out, which == 2 ? MS_STRONG : MS_EM, true, NULL, 0);
			} else {
				if (nopen < 2)
					order[nopen++] = which;
				*open = true;
				span(&out, which == 2 ? MS_STRONG : MS_EM, false, NULL, 0);
			}
			i += (size_t)which;
			continue;
		}
		if (c == '!' && i + 1 < n && s[i + 1] == '[') {
			err(p, line, "an image: not in the documentation's Markdown");
			i++;
			continue;
		}
		if (c == '[' && !linkend) {
			/* a link if "](target)" follows; else a plain "[" */
			size_t j = i + 1;
			bool incode = false;
			while (j < n && (incode || s[j] != ']')) {
				if (s[j] == '`')
					incode = !incode;
				else if (s[j] == '\\' && j + 1 < n)
					j++;
				j++;
			}
			if (j + 1 < n && s[j] == ']' && s[j + 1] == '[') {
				err(p, line, "a reference link: write [text](target)");
			} else if (j + 1 < n && s[j] == ']' && s[j + 1] == '(') {
				const char *tg = s + j + 2;
				const char *e = memchr(tg, ')', n - (size_t)(tg - s));
				if (!e) {
					err(p, line, "a link whose target is never closed");
				} else {
					flushtext(&out, &t);
					span(&out, MS_LINK, false, tg, (size_t)(e - tg));
					target = tg;
					targetn = (size_t)(e - tg);
					linkend = j;
					i++;
					continue;
				}
			}
			bufputc(&t, c);
			i++;
			continue;
		}
		if (c == '<' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '!' || (s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z'))) {
			err(p, line, "HTML (or an <autolink>): not in the documentation's Markdown; escape the < as \\<");
			bufputc(&t, c);
			i++;
			continue;
		}
		bufputc(&t, c);
		i++;
	}
	flushtext(&out, &t);
	if (em || strong)
		err(p, line, "emphasis that is never closed (a literal * is written \\*)");
	if (linkend)
		err(p, line, "a link that is never closed");
	return out;
}

void md_plain(Buf *b, const MdSpans *s) {
	for (size_t i = 0; i < s->n; i++)
		if (s->v[i].kind == MS_TEXT || s->v[i].kind == MS_CODE)
			bufputs(b, s->v[i].s);
}

char *md_slug(const char *text) {
	Buf b = {0};
	for (const unsigned char *s = (const unsigned char *)text; *s; s++) {
		int c = *s;
		if (c >= 'A' && c <= 'Z')
			bufputc(&b, c - 'A' + 'a');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80)
			bufputc(&b, c);
		else if (c == ' ')
			bufputc(&b, '-');
	}
	if (!b.b)
		bufputs(&b, "");
	return b.b;
}

/* ------------------------------------------------------------------ */
/* Blocks */

static MdBlock *newblock(Parser *p, MdBlockKind k, int line, int depth) {
	MdBlock b = {0};
	b.kind = k, b.line = line, b.depth = depth;
	VPUSH(p->d->blocks, b);
	return &p->d->blocks.v[p->d->blocks.n - 1];
}

/* endtext completes the open text block: its inline markup, and a
 * heading's anchor. */
static void endtext(Parser *p) {
	if (p->textblock < 0)
		return;
	MdBlock *b = &p->d->blocks.v[p->textblock];
	b->spans = inlines(p, b->line, p->text.b ? p->text.b : "", p->text.n);
	if (b->kind == MB_HEADING) {
		Buf plain = {0};
		md_plain(&plain, &b->spans);
		char *slug = md_slug(plain.b ? plain.b : "");
		size_t sn = strlen(slug);
		intptr_t seen = (intptr_t)strmap_get(&p->anchors, slug, sn);
		strmap_put(&p->anchors, slug, sn, (void *)(seen + 1));
		if (seen) {
			Buf a = {0};
			bufprintf(&a, "%s-%ld", slug, (long)seen);
			b->anchor = a.b;
		} else
			b->anchor = slug;
		if (!*p->d->title)
			p->d->title = plain.b ? plain.b : "";
	}
	p->text.n = 0;
	if (p->text.b)
		p->text.b[0] = 0;
	p->textblock = -1;
}

static void addtext(Parser *p, const char *s, size_t n) {
	while (n && s[n - 1] == ' ')
		n--;
	if (p->text.n)
		bufputc(&p->text, ' ');
	bufadd(&p->text, s, n);
}

/* itemmarker: the width of a list marker at s ("- ", "1. "), or 0. */
static int itemmarker(const char *s, size_t n, bool *ordered, int *number) {
	if (n >= 2 && s[0] == '-' && s[1] == ' ') {
		*ordered = false;
		return 2;
	}
	size_t i = 0;
	int v = 0;
	while (i < n && i < 9 && s[i] >= '0' && s[i] <= '9')
		v = v * 10 + (s[i++] - '0');
	if (i > 0 && i + 1 < n && s[i] == '.' && s[i + 1] == ' ') {
		*ordered = true;
		*number = v;
		return (int)i + 2;
	}
	return 0;
}

static bool isrule(const char *s, size_t n, char c) {
	size_t k = 0;
	while (k < n && s[k] == c)
		k++;
	while (k < n && s[k] == ' ')
		k++;
	return k == n && n >= 3 && s[0] == c;
}

/* splitrow splits a table row "| a | b |" into its cells. */
typedef VEC(char *) Row;

static Row splitrow(const char *s, size_t n) {
	Row cells = {0};
	size_t i = 0;
	if (i < n && s[i] == '|')
		i++;
	while (n > i && s[n - 1] == ' ')
		n--;
	if (n > i && s[n - 1] == '|' && !(n >= 2 && s[n - 2] == '\\'))
		n--;
	size_t start = i;
	bool incode = false;
	for (; i <= n; i++) {
		if (i < n && s[i] == '\\') {
			i++;
			continue;
		}
		if (i < n && s[i] == '`')
			incode = !incode;
		if (i == n || (s[i] == '|' && !incode)) {
			size_t a = start, b = i;
			while (a < b && s[a] == ' ')
				a++;
			while (b > a && s[b - 1] == ' ')
				b--;
			VPUSH(cells, xstrndup(s + a, b - a));
			start = i + 1;
		}
	}
	return cells;
}

static bool isseparator(char *const *cells, size_t n) {
	for (size_t i = 0; i < n; i++) {
		const char *c = cells[i];
		if (*c == ':')
			c++;
		if (*c != '-')
			return false;
		while (*c == '-')
			c++;
		if (*c == ':')
			c++;
		if (*c)
			return false;
	}
	return n > 0;
}

MdDoc *md_parse(const char *src, size_t n) {
	MdDoc *d = xcalloc(1, sizeof *d);
	d->title = "";
	Parser P = {d, {{0}}, 0, {0}, -1, {0}}, *p = &P;
	/* the lines */
	VEC(const char *) ls = {0};
	VEC(size_t) lens = {0};
	for (size_t i = 0; i < n;) {
		const char *e = memchr(src + i, '\n', n - i);
		size_t len = e ? (size_t)(e - (src + i)) : n - i;
		VPUSH(ls, src + i);
		VPUSH(lens, len);
		i += len + 1;
	}
	bool prevblank = true;
	MdBlock *fence = NULL;
	int fenceindent = 0, fenceline = 0;
	Buf code = {0};
	for (size_t li = 0; li < ls.n; li++) {
		const char *s = ls.v[li];
		size_t len = lens.v[li];
		int line = (int)li + 1;
		if (len && s[len - 1] == '\r')
			len--;
		int indent = 0;
		while ((size_t)indent < len && s[indent] == ' ')
			indent++;
		const char *r = s + indent;
		size_t rn = len - (size_t)indent;
		if (fence) {
			if (rn >= 3 && !strncmp(r, "```", 3) && isrule(r, rn, '`')) {
				fence->code = code.b ? xstrndup(code.b, code.n) : "";
				code.n = 0;
				fence = NULL;
				prevblank = false;
				continue;
			}
			int strip = indent < fenceindent ? indent : fenceindent;
			bufadd(&code, s + strip, len - (size_t)strip);
			bufputc(&code, '\n');
			continue;
		}
		if (memchr(s, '\t', len))
			err(p, line, "a tab: use spaces outside code");
		if (rn == 0) {
			endtext(p);
			prevblank = true;
			continue;
		}
		bool ordered = false;
		int number = 0;
		int mw = itemmarker(r, rn, &ordered, &number);
		bool start = mw || r[0] == '#' || r[0] == '|' || (rn >= 3 && !strncmp(r, "```", 3));
		if (p->textblock >= 0 && !prevblank && !start) {
			if (isrule(r, rn, '=') || isrule(r, rn, '-'))
				err(p, line, "a setext heading or a rule: use # headings");
			addtext(p, r, rn);
			continue;
		}
		endtext(p);
		prevblank = false;
		if (mw) {
			while (p->nlists && indent < p->lists[p->nlists - 1].mi)
				p->nlists--;
			if (p->nlists && indent == p->lists[p->nlists - 1].mi) {
				if (p->lists[p->nlists - 1].ordered != ordered)
					p->lists[p->nlists - 1].ordered = ordered; /* another list begins */
			} else if (p->nlists && indent >= p->lists[p->nlists - 1].ci) {
				if (p->nlists == 2)
					err(p, line, "lists nest one level only");
				else
					p->lists[p->nlists++] = (Level){indent, indent + mw, ordered};
			} else if (p->nlists) {
				err(p, line, "a list item indented between levels");
			} else if (indent) {
				err(p, line, "an indented list item outside a list");
				p->lists[p->nlists++] = (Level){indent, indent + mw, ordered};
			} else
				p->lists[p->nlists++] = (Level){0, mw, ordered};
			int lv = p->nlists - 1;
			p->lists[lv].ci = indent + mw;
			MdBlock *b = newblock(p, MB_ITEM, line, lv);
			b->level = lv, b->ordered = ordered, b->number = number;
			p->textblock = (int)d->blocks.n - 1;
			addtext(p, r + mw, rn - (size_t)mw);
			continue;
		}
		while (p->nlists && indent < p->lists[p->nlists - 1].ci)
			p->nlists--;
		int depth = p->nlists;
		int want = p->nlists ? p->lists[p->nlists - 1].ci : 0;
		if (indent >= want + 4)
			err(p, line, "indented code: use a ``` fence");
		else if (indent != want)
			err(p, line, "unexpected indentation");
		if (rn >= 3 && !strncmp(r, "```", 3)) {
			fence = newblock(p, MB_CODE, line, depth);
			const char *l = r + 3;
			size_t ln = rn - 3;
			while (ln && *l == ' ')
				l++, ln--;
			while (ln && l[ln - 1] == ' ')
				ln--;
			if (memchr(l, '`', ln))
				err(p, line, "a ` in a fence's language");
			fence->lang = xstrndup(l, ln);
			fenceindent = indent;
			fenceline = line;
			continue;
		}
		if (r[0] == '#') {
			int k = 0;
			while ((size_t)k < rn && r[k] == '#')
				k++;
			if ((size_t)k < rn && r[k] != ' ') {
				err(p, line, "a heading needs a space after its #s");
			}
			if (k > 4)
				err(p, line, "headings go to #### only");
			if (depth)
				err(p, line, "a heading inside a list");
			MdBlock *b = newblock(p, MB_HEADING, line, depth);
			b->level = k > 4 ? 4 : k;
			p->textblock = (int)d->blocks.n - 1;
			size_t off = (size_t)k;
			while (off < rn && r[off] == ' ')
				off++;
			addtext(p, r + off, rn - off);
			endtext(p);
			continue;
		}
		if (r[0] == '|') {
			MdBlock *b = newblock(p, MB_TABLE, line, depth);
			VEC(Row) rows = {0};
			size_t lj = li;
			for (; lj < ls.n; lj++) {
				const char *t = ls.v[lj];
				size_t tn = lens.v[lj];
				if (tn && t[tn - 1] == '\r')
					tn--;
				int ti = 0;
				while ((size_t)ti < tn && t[ti] == ' ')
					ti++;
				if ((size_t)ti >= tn || t[ti] != '|')
					break;
				if (memchr(t, '\t', tn) && lj != li)
					err(p, (int)lj + 1, "a tab: use spaces outside code");
				Row row = splitrow(t + ti, tn - (size_t)ti);
				VPUSH(rows, row);
			}
			li = lj - 1;
			if (rows.n < 2 || !isseparator(rows.v[1].v, rows.v[1].n)) {
				err(p, line, "a table needs a header row and a |---| row");
				continue;
			}
			b->ncols = (int)rows.v[0].n;
			b->nrows = (int)rows.n - 1;
			b->cells = xcalloc((size_t)b->nrows * (size_t)b->ncols, sizeof *b->cells);
			for (size_t ri = 0, out = 0; ri < rows.n; ri++) {
				if (ri == 1)
					continue;
				if ((int)rows.v[ri].n != b->ncols)
					err(p, line + (int)ri, "a table row whose cells differ in number from the header's");
				for (int ci = 0; ci < b->ncols; ci++) {
					const char *cell = ci < (int)rows.v[ri].n ? rows.v[ri].v[ci] : "";
					b->cells[out * (size_t)b->ncols + (size_t)ci] = inlines(p, line + (int)ri, cell, strlen(cell));
				}
				out++;
			}
			continue;
		}
		if (r[0] == '>')
			err(p, line, "a block quote: not in the documentation's Markdown");
		else if (r[0] == '<')
			err(p, line, "HTML: not in the documentation's Markdown");
		else if (rn >= 2 && (r[0] == '*' || r[0] == '+') && r[1] == ' ')
			err(p, line, "a list item marked with * or +: use -");
		else if (isrule(r, rn, '-') || isrule(r, rn, '*') || isrule(r, rn, '_') || isrule(r, rn, '='))
			err(p, line, "a rule: not in the documentation's Markdown");
		newblock(p, MB_PARA, line, depth);
		p->textblock = (int)d->blocks.n - 1;
		addtext(p, r, rn);
	}
	if (fence) {
		err(p, fenceline, "a ``` fence that is never closed");
		fence->code = code.b ? xstrndup(code.b, code.n) : "";
	}
	endtext(p);
	return d;
}

const char *md_folded(const char *lang) {
	if (!lang || strncmp(lang, "folded", 6) || (lang[6] && lang[6] != ' '))
		return NULL;
	const char *t = lang + 6;
	while (*t == ' ')
		t++;
	return t;
}
