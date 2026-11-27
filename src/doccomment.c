/*
 * Doc comments (doccomment.h): comment lines to the documentation's
 * Markdown, by Go's rules, without recursion.
 */
#include "doccomment.h"

#include <stdlib.h>
#include <string.h>

/* text is a comment line's text: the # taken off, a ":" after it (the
 * IPL's summary mark) too, tabs expanded as in the file (the # in
 * column 1). NULL for a rule. */
static char *text(const char *line) {
	const char *s = line;
	int col = 0;
	if (*s == '#')
		s++, col++;
	if (*s == ':')
		s++, col++;
	bool rule = true;
	for (const char *p = s; *p; p++)
		if (!strchr("#=-*_ \t\r", *p))
			rule = false;
	if (rule && strspn(s + strspn(s, " \t"), "#=-*_") >= 3)
		return NULL;
	Buf b = {0};
	for (; *s && *s != '\r' && *s != '\n'; s++) {
		if (*s == '\t') {
			do
				bufputc(&b, ' '), col++;
			while (col % 8);
		} else
			bufputc(&b, *s), col++;
	}
	while (b.n && b.b[b.n - 1] == ' ')
		b.b[--b.n] = 0;
	return b.b ? b.b : xstrndup("", 0);
}

static size_t indent(const char *s) { return strspn(s, " "); }

/* listmarker: the width of a list item's marker at s, or 0. */
static size_t listmarker(const char *s) {
	if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ')
		return 2;
	size_t i = 0;
	while (s[i] >= '0' && s[i] <= '9')
		i++;
	if (i && i < 4 && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ')
		return i + 2;
	return 0;
}

/* esc appends literal text, escaped so that Markdown reads it as text:
 * its punctuation, and at the start of a line what would begin a block. */
static void esc(Buf *b, const char *s, bool linestart) {
	if (linestart && (strchr("#|>-=+", s[0]) || listmarker(s))) { /* * and _ are escaped below */
		size_t k = strspn(s, "0123456789");
		bufadd(b, s, k);
		s += k;
		bufputc(b, '\\');
	}
	for (; *s; s++) {
		if (strchr("\\`*[]<_", *s))
			bufputc(b, '\\');
		bufputc(b, *s);
	}
}

/* sep ends what b holds with a blank line, before a new block. */
static void sep(Buf *b) {
	while (b->n && !(b->n >= 2 && b->b[b->n - 1] == '\n' && b->b[b->n - 2] == '\n'))
		bufputc(b, '\n');
}

/* code appends lines [from, to) of t as a code block, less their
 * common indentation; blank lines at its end are dropped. */
static void code(Buf *b, char *const *t, size_t from, size_t to) {
	while (to > from && !*t[to - 1])
		to--;
	size_t min = (size_t)-1;
	for (size_t i = from; i < to; i++)
		if (*t[i] && indent(t[i]) < min)
			min = indent(t[i]);
	sep(b);
	bufputs(b, "```\n");
	for (size_t i = from; i < to; i++) {
		const char *s = *t[i] ? t[i] + min : t[i];
		if (!strncmp(s, "```", 3))
			bufputc(b, ' ');
		bufputs(b, s);
		bufputc(b, '\n');
	}
	bufputs(b, "```\n");
}

char *doccomment_md(char *const *lines, size_t n) {
	VEC(char *) t = {0};
	for (size_t i = 0; i < n; i++) {
		char *s = text(lines[i]);
		if (s)
			VPUSH(t, s);
	}
	/* the comment's own indentation: the least of its lines' */
	size_t base = (size_t)-1;
	for (size_t i = 0; i < t.n; i++)
		if (t.v[i][0] && indent(t.v[i]) < base)
			base = indent(t.v[i]);
	if (base == (size_t)-1)
		return xstrndup("", 0);
	for (size_t i = 0; i < t.n; i++)
		t.v[i] = t.v[i][0] ? t.v[i] + base : t.v[i];
	Buf b = {0};
	enum { None, Para, Item } st = None;
	for (size_t i = 0; i < t.n;) {
		const char *s = t.v[i];
		if (!*s) {
			st = None;
			i++;
			continue;
		}
		size_t in = indent(s), mk = listmarker(s);
		if (st == Item && in > 0 && !listmarker(s + in)) {
			/* an item's continuation */
			bufputc(&b, ' ');
			esc(&b, s + in, false);
			i++;
			continue;
		}
		if (in > 0) {
			/* code: indented lines, and blank lines between them */
			size_t j = i;
			while (j < t.n && (!*t.v[j] || indent(t.v[j]) > 0))
				j++;
			code(&b, t.v, i, j);
			st = None;
			i = j;
			continue;
		}
		if (s[0] == '#' && s[1] == ' ') {
			sep(&b);
			bufputs(&b, "### ");
			esc(&b, s + 2, false);
			bufputc(&b, '\n');
			st = None;
			i++;
			continue;
		}
		if (mk) {
			if (st != Item)
				sep(&b);
			else
				bufputc(&b, '\n');
			if (s[0] >= '0' && s[0] <= '9')
				bufprintf(&b, "%d. ", atoi(s)); /* numbered, as written */
			else
				bufputs(&b, "- ");
			esc(&b, s + mk, false);
			st = Item;
			i++;
			continue;
		}
		if (st == Para)
			bufputc(&b, ' ');
		else
			sep(&b);
		esc(&b, s, st != Para);
		st = Para;
		i++;
	}
	while (b.n && b.b[b.n - 1] == '\n')
		b.b[--b.n] = 0;
	return b.b ? b.b : xstrndup("", 0);
}

/* declaration: the kind of declaration a line begins, or NULL. */
static const char *declaration(const char *s, size_t n, const char **name, size_t *namelen) {
	static const char *const kinds[] = {"procedure", "record", "global"};
	for (size_t k = 0; k < 3; k++) {
		size_t kl = strlen(kinds[k]);
		if (n > kl && !strncmp(s, kinds[k], kl) && (s[kl] == ' ' || s[kl] == '\t')) {
			const char *p = s + kl;
			while (p < s + n && (*p == ' ' || *p == '\t'))
				p++;
			const char *q = p;
			while (q < s + n && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_'))
				q++;
			if (q == p)
				return NULL;
			*name = p, *namelen = (size_t)(q - p);
			return kinds[k];
		}
	}
	return NULL;
}

DocDecls doccomment_file(const char *src, size_t n, char **filedoc) {
	DocDecls v = {0};
	*filedoc = xstrndup("", 0);
	VEC(char *) cmt = {0}; /* the comment lines just above */
	int cmtline = 0;       /* the first of them's line */
	bool top = true;       /* no code yet */
	bool gap = false;      /* a blank line since the comment */
	int line = 0;
	for (size_t i = 0; i < n;) {
		const char *e = memchr(src + i, '\n', n - i);
		size_t len = e ? (size_t)(e - (src + i)) : n - i;
		const char *s = src + i;
		line++;
		i += len + 1;
		size_t sp = 0;
		while (sp < len && (s[sp] == ' ' || s[sp] == '\t'))
			sp++;
		if (sp == len || (len - sp == 1 && s[sp] == '\r')) {
			if (cmt.n)
				gap = true;
			continue;
		}
		if (s[sp] == '#') {
			if (gap) {
				/* a comment before a blank line: the file's, at its top */
				if (top && !**filedoc)
					*filedoc = doccomment_md(cmt.v, cmt.n);
				cmt.n = 0;
				gap = false;
			}
			if (!cmt.n)
				cmtline = line;
			VPUSH(cmt, xstrndup(s + sp, len - sp));
			continue;
		}
		const char *name;
		size_t nl;
		const char *kind = sp == 0 ? declaration(s, len, &name, &nl) : NULL;
		if (top && cmt.n && !**filedoc && (gap || !kind)) {
			/* the comment at the top, before a blank line or code that is not
			 * a declaration: the file's */
			*filedoc = doccomment_md(cmt.v, cmt.n);
			cmt.n = 0;
		}
		if (kind) {
			if (gap)
				cmt.n = 0;
			size_t above = cmt.n; /* the comment lines above, before a "#:" */
			/* the heading: the line, to a comment on it */
			size_t hl = len;
			const char *hash = memchr(s, '#', len);
			if (hash) {
				/* an IPL summary, "#:", on the line */
				if (hash + 1 < s + len && hash[1] == ':')
					VPUSH(cmt, xstrndup(hash, (size_t)(s + len - hash)));
				hl = (size_t)(hash - s);
			}
			while (hl && (s[hl - 1] == ' ' || s[hl - 1] == '\t' || s[hl - 1] == '\r'))
				hl--;
			char **lines = xcalloc(above + 1, sizeof *lines);
			memcpy(lines, cmt.v, above * sizeof *lines);
			DocDecl d = {(char *)kind, xstrndup(name, nl), xstrndup(s, hl), doccomment_md(cmt.v, cmt.n), line,
				lines, above, cmtline};
			VPUSH(v, d);
		}
		top = false;
		cmt.n = 0;
		gap = false;
	}
	if (top && cmt.n && !**filedoc)
		*filedoc = doccomment_md(cmt.v, cmt.n);
	return v;
}

static bool identchar(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* inheading reports whether the identifier w (n bytes) is in the
 * declaration's heading: one of a global's names, say. */
static bool inheading(const char *decl, const char *w, size_t n) {
	char *word = xstrndup(w, n);
	for (const char *s = decl; (s = strstr(s, word));) {
		if ((s == decl || !identchar((unsigned char)s[-1])) && !identchar((unsigned char)s[n]))
			return true;
		s++;
	}
	return false;
}

static void warn(DocWarnings *out, int line, const char *msg) {
	DocWarning w = {line, xstrndup(msg, strlen(msg))};
	VPUSH(*out, w);
}

void doccomment_lint(const DocDecl *d, DocNamed *named, void *ctx, DocWarnings *out) {
	/* the lines' text, and each one's line in the file */
	VEC(char *) t = {0};
	VEC(int) at = {0};
	for (size_t i = 0; i < d->nlines; i++) {
		char *s = text(d->lines[i]);
		if (s) {
			VPUSH(t, s);
			VPUSH(at, d->firstline + (int)i);
		}
	}
	size_t base = (size_t)-1;
	for (size_t i = 0; i < t.n; i++)
		if (t.v[i][0] && indent(t.v[i]) < base)
			base = indent(t.v[i]);
	if (base == (size_t)-1)
		return;
	for (size_t i = 0; i < t.n; i++)
		t.v[i] = t.v[i][0] ? t.v[i] + base : t.v[i];

	/* a comment that begins with another declaration's name */
	for (size_t i = 0; i < t.n; i++) {
		const char *s = t.v[i];
		if (!*s)
			continue;
		size_t n = 0;
		while (identchar((unsigned char)s[n]))
			n++;
		if (n && !(s[0] >= '0' && s[0] <= '9') && s[n] == ' ' && !inheading(d->decl, s, n) && named(ctx, s, n)) {
			Buf m = {0};
			bufprintf(&m, "doc comment begins with \"%.*s\", another declaration's name, but documents \"%s\"", (int)n, s, d->name);
			warn(out, at.v[i], m.b);
		}
		break;
	}

	/* lines that will not read as they look: as the reader reads them */
	enum { None, Para, Item, Code } st = None;
	for (size_t i = 0; i < t.n; i++) {
		const char *s = t.v[i];
		if (!*s) {
			if (st != Code)
				st = None;
			continue;
		}
		size_t in = indent(s);
		bool mk = listmarker(s) != 0, head = s[0] == '#' && s[1] == ' ';
		if (st == Para && in > 0 && in <= 3) {
			warn(out, at.v[i], "doc comment: a line indented a little directly after text is shown as code; "
				"put a blank line before code, or take the indentation off to continue the text");
			st = Code;
			continue;
		}
		if (st == Item && in == 0 && !mk && !head) {
			warn(out, at.v[i], "doc comment: a line directly after a list item ends the list; "
				"indent it to continue the item, or put a blank line before it");
			st = Para;
			continue;
		}
		if (in > 0 && st != Item)
			st = Code;
		else if (head)
			st = None;
		else if (mk || (st == Item && in > 0))
			st = Item;
		else
			st = Para;
	}
}

char *doccomment_firstword(const DocDecl *d) {
	for (size_t i = 0; i < d->nlines; i++) {
		char *s = text(d->lines[i]);
		if (!s || !*s)
			continue;
		s += indent(s);
		size_t n = 0;
		while (identchar((unsigned char)s[n]))
			n++;
		return n && !(s[0] >= '0' && s[0] <= '9') && s[n] == ' ' ? xstrndup(s, n) : NULL;
	}
	return NULL;
}
