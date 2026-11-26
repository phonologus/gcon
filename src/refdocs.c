/*
 * Reference documentation (refdocs.h), as icontools' builtin/docs.go
 * parses and renders it. Its regular expressions are written here as
 * the small scanners they amount to.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "refdocs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef VEC(char *) CharVec;

static const char site[] = "https://www2.cs.arizona.edu/icon/";

/* The reference pages, relative to the site. */
static const char *const refpages[] = {
	"refernce/funcad.htm", "refernce/funcej.htm", "refernce/funcko.htm", "refernce/funcpr.htm",
	"refernce/funcsz.htm", "refernce/keywlist.htm",
	"docs/ipd281/gipd_a.htm", /* graphics procedures */
	"docs/ipd281/gipd_b.htm", /* graphics keywords */
};
enum { NPages = sizeof refpages / sizeof refpages[0] };

typedef struct Str {
	const char *s;
	size_t n;
} Str;

static bool isspace_(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static bool isword(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

/* ci: s at i starts with the ASCII pattern p, ignoring case. */
static bool ci(Str s, size_t i, const char *p) {
	size_t n = strlen(p);
	return i + n <= s.n && strncasecmp(s.s + i, p, n) == 0;
}

/* striptags removes <...> (tagRE). */
static Str striptags(Str s) {
	Buf b = {0};
	for (size_t i = 0; i < s.n; i++) {
		if (s.s[i] == '<') {
			const char *e = memchr(s.s + i, '>', s.n - i);
			if (e) {
				i = (size_t)(e - s.s);
				continue;
			}
		}
		bufputc(&b, s.s[i]);
	}
	Str r = {b.b ? b.b : "", b.n};
	return r;
}

/* unescape decodes the HTML character references (html.UnescapeString),
 * the named ones these pages use, and numeric ones. */
static Str unescape(Str s) {
	static const struct {
		const char *name, *text;
	} ents[] = {
		{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
		{"nbsp", "\xc2\xa0"}, {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"},
	};
	Buf b = {0};
	for (size_t i = 0; i < s.n; i++) {
		if (s.s[i] == '&') {
			if (i + 2 < s.n && s.s[i + 1] == '#') {
				size_t j = i + 2;
				int base = 10;
				if (j < s.n && (s.s[j] == 'x' || s.s[j] == 'X'))
					base = 16, j++;
				long v = 0;
				size_t k = j;
				while (k < s.n && ((base == 10 && s.s[k] >= '0' && s.s[k] <= '9') ||
							  (base == 16 && strchr("0123456789abcdefABCDEF", s.s[k]) && s.s[k]))) {
					v = v * base + (s.s[k] <= '9' ? s.s[k] - '0' : (s.s[k] | 0x20) - 'a' + 10);
					k++;
				}
				if (k > j) {
					if (k < s.n && s.s[k] == ';')
						k++;
					uint8_t u[4];
					int w = utf8encode(u, v > 0 && v <= 0x10FFFF ? (int32_t)v : 0xFFFD);
					bufadd(&b, u, (size_t)w);
					i = k - 1;
					continue;
				}
			}
			bool done = false;
			for (size_t e = 0; e < sizeof ents / sizeof ents[0] && !done; e++) {
				size_t n = strlen(ents[e].name);
				if (i + 1 + n < s.n + 1 && !strncmp(s.s + i + 1, ents[e].name, n) && i + 1 + n < s.n &&
				    s.s[i + 1 + n] == ';') {
					bufputs(&b, ents[e].text);
					i += n + 1;
					done = true;
				}
			}
			if (done)
				continue;
		}
		bufputc(&b, s.s[i]);
	}
	Str r = {b.b ? b.b : "", b.n};
	return r;
}

/* fields joins s's space-separated words with single spaces. */
static Str fields(Str s) {
	Buf b = {0};
	size_t i = 0;
	while (i < s.n) {
		while (i < s.n && isspace_((unsigned char)s.s[i]))
			i++;
		size_t j = i;
		while (j < s.n && !isspace_((unsigned char)s.s[j]))
			j++;
		if (j > i) {
			if (b.n)
				bufputc(&b, ' ');
			bufadd(&b, s.s + i, j - i);
		}
		i = j;
	}
	Str r = {b.b ? b.b : "", b.n};
	return r;
}

static Str trimspace(Str s) {
	while (s.n && isspace_((unsigned char)*s.s))
		s.s++, s.n--;
	while (s.n && isspace_((unsigned char)s.s[s.n - 1]))
		s.n--;
	return s;
}

/* text converts an HTML fragment to plain text on one line. */
static Str text(Str s) { return fields(unescape(striptags(s))); }

static Str sub(Str s, size_t a, size_t z) {
	Str r = {s.s + a, z - a};
	return r;
}

/* markdown converts an entry body to Markdown: paragraphs are filled,
 * <CODE> becomes `code`, and <PRE> blocks become code blocks, except
 * for "See also" lines, which become text. */
static void para(Str s, CharVec *out) {
	/* <CODE>x</CODE> -> `x` (codeRE, non-greedy) */
	Buf c = {0};
	for (size_t i = 0; i < s.n; i++) {
		if (ci(s, i, "<CODE>")) {
			size_t j = i + 6;
			while (j < s.n && !ci(s, j, "</CODE>"))
				j++;
			if (j < s.n) {
				bufputc(&c, '`');
				bufadd(&c, s.s + i + 6, j - i - 6);
				bufputc(&c, '`');
				i = j + 6;
				continue;
			}
		}
		bufputc(&c, s.s[i]);
	}
	Str cs = {c.b ? c.b : "", c.n};
	/* split at <BR> or <P> (brRE) */
	size_t start = 0;
	for (size_t i = 0; i <= cs.n; i++) {
		size_t skip = 0;
		if (i == cs.n)
			skip = 1;
		else if (ci(cs, i, "<BR>"))
			skip = 4;
		else if (ci(cs, i, "<P>"))
			skip = 3;
		if (!skip)
			continue;
		Str t = text(sub(cs, start, i));
		if (t.n)
			VPUSH(*out, xstrndup(t.s, t.n));
		if (i == cs.n)
			break;
		i += skip - 1;
		start = i + 1;
	}
}

static char *markdown(Str body) {
	CharVec out = {0};
	Str rest = body;
	for (;;) {
		size_t a = 0;
		while (a < rest.n && !ci(rest, a, "<PRE>"))
			a++;
		size_t z = a < rest.n ? a + 5 : rest.n;
		while (z < rest.n && !ci(rest, z, "</PRE>"))
			z++;
		if (a >= rest.n || z >= rest.n) {
			para(rest, &out);
			break;
		}
		para(sub(rest, 0, a), &out);
		Str inner = unescape(striptags(sub(rest, a + 5, z)));
		VEC(Str) code = {0};
		CharVec also = {0};
		size_t ls = 0;
		for (size_t i = 0; i <= inner.n; i++) {
			if (i < inner.n && inner.s[i] != '\n')
				continue;
			Str l = sub(inner, ls, i);
			ls = i + 1;
			while (l.n && (l.s[l.n - 1] == ' ' || l.s[l.n - 1] == '\t' || l.s[l.n - 1] == '\r'))
				l.n--;
			Str t = trimspace(l);
			if (t.n >= 8 && !strncmp(t.s, "See also", 8))
				VPUSH(also, xstrndup(t.s, t.n));
			else if (t.n || code.n)
				VPUSH(code, l);
		}
		while (code.n && trimspace(code.v[code.n - 1]).n == 0)
			code.n--;
		if (code.n) {
			Buf b = {0};
			bufputs(&b, "```\n");
			for (size_t i = 0; i < code.n; i++) {
				if (i)
					bufputc(&b, '\n');
				bufadd(&b, code.v[i].s, code.v[i].n);
			}
			bufputs(&b, "\n```");
			VPUSH(out, b.b);
		}
		for (size_t i = 0; i < also.n; i++)
			VPUSH(out, also.v[i]);
		rest = sub(rest, z + 6, rest.n);
	}
	Buf b = {0};
	for (size_t i = 0; i < out.n; i++) {
		if (i)
			bufputs(&b, "\n\n");
		bufputs(&b, out.v[i]);
	}
	return b.b ? b.b : xstrndup("", 0);
}

/* entry renders a title and HTML body as Markdown. */
static char *entry(Str title, Str body) {
	Str sig = title, summary = {"", 0};
	for (size_t i = 0; i + 1 < title.n; i++)
		if (title.s[i] == '-' && title.s[i + 1] == '-') {
			sig = sub(title, 0, i);
			summary = sub(title, i + 2, title.n);
			break;
		}
	sig = fields(sig);
	Buf s = {0};
	bufadd(&s, sig.s, sig.n);
	char *p = s.b ? strstr(s.b, " (") : NULL;
	if (p)
		memmove(p, p + 1, strlen(p + 1) + 1), s.n--;
	Buf b = {0};
	bufprintf(&b, "**%s**", s.b ? s.b : "");
	summary = trimspace(summary);
	if (summary.n) {
		bufputs(&b, " \xe2\x80\x94 ");
		bufadd(&b, summary.s, summary.n);
	}
	char *md = markdown(body);
	if (*md) {
		bufputs(&b, "\n\n");
		bufputs(&b, md);
	}
	return b.b;
}

/* parseentries parses a page of entries separated by <HR>, each a title
 * line ("name(args) : result -- summary" or "&name : type -- summary")
 * and a body. Function entries also carry a NAME anchor. */
static bool anchor(Str s) {
	for (size_t i = 0; i < s.n; i++) {
		if (!ci(s, i, "<A"))
			continue;
		size_t j = i + 2;
		if (j >= s.n || !isspace_((unsigned char)s.s[j]))
			continue;
		while (j < s.n && isspace_((unsigned char)s.s[j]))
			j++;
		if (!ci(s, j, "NAME"))
			continue;
		j += 4;
		while (j < s.n && isspace_((unsigned char)s.s[j]))
			j++;
		if (j >= s.n || s.s[j] != '=')
			continue;
		j++;
		while (j < s.n && isspace_((unsigned char)s.s[j]))
			j++;
		if (j >= s.n || s.s[j] != '"')
			continue;
		size_t k = j + 1;
		while (k < s.n && s.s[k] != '"')
			k++;
		if (k >= s.n || k == j + 1)
			continue;
		k++;
		while (k < s.n && isspace_((unsigned char)s.s[k]))
			k++;
		if (k < s.n && s.s[k] == '>')
			return true;
	}
	return false;
}

static void parseentries(Str page, RefDocs *d) {
	size_t start = 0;
	for (size_t i = 0; i <= page.n; i++) {
		if (i < page.n && !ci(page, i, "<HR>"))
			continue;
		Str chunk = sub(page, start, i);
		start = i + 4;
		/* the title ends at </H3> or <BR> */
		size_t a = 0, len = 0;
		for (; a < chunk.n; a++) {
			if (ci(chunk, a, "</H3>")) {
				len = 5;
				break;
			}
			if (ci(chunk, a, "<BR>")) {
				len = 4;
				break;
			}
		}
		if (a < chunk.n) {
			Str title = text(sub(chunk, 0, a)), body = sub(chunk, a + len, chunk.n);
			if (title.n > 1 && title.s[0] == '&' && isword((unsigned char)title.s[1])) {
				size_t k = 1;
				while (k < title.n && isword((unsigned char)title.s[k]))
					k++;
				strmap_put(&d->keywords, title.s + 1, k - 1, entry(title, body));
			} else {
				size_t k = 0;
				while (k < title.n && isword((unsigned char)title.s[k]))
					k++;
				size_t m = k;
				while (m < title.n && isspace_((unsigned char)title.s[m]))
					m++;
				if (k > 0 && m < title.n && title.s[m] == '(' && anchor(sub(chunk, 0, a)))
					strmap_put(&d->functions, title.s, k, entry(title, body));
			}
		}
		if (i == page.n)
			break;
		i += 3;
	}
}

/* parsekeywordlist parses keywlist.htm: a <PRE> block of lines
 * "&name : type  # summary", with continuation lines "  # more". */
static void parsekeywordlist(Str page, StrMap *kws) {
	Str all = unescape(striptags(page));
	char *name = NULL;
	Str sig = {"", 0};
	Buf summary = {0};
	size_t ls = 0;
	for (size_t i = 0; i <= all.n; i++) {
		if (i < all.n && all.s[i] != '\n')
			continue;
		Str t = trimspace(sub(all, ls, i));
		ls = i + 1;
		Str code = t, comment = {"", 0};
		const char *h = memchr(t.s, '#', t.n);
		if (h) {
			code = sub(t, 0, (size_t)(h - t.s));
			comment = sub(t, (size_t)(h - t.s) + 1, t.n);
		}
		code = trimspace(code), comment = trimspace(comment);
		if (code.n && code.s[0] == '&') {
			if (name) {
				Buf e = {0};
				bufprintf(&e, "**%.*s** \xe2\x80\x94 %s", (int)sig.n, sig.s, summary.b ? summary.b : "");
				strmap_put(kws, name, strlen(name), e.b);
			}
			Str f = fields(code);
			size_t k = 0;
			while (k < f.n && f.s[k] != ' ')
				k++;
			name = xstrndup(f.s + 1, k - 1);
			sig = f;
			summary.n = 0;
			bufadd(&summary, comment.s, comment.n);
			if (!summary.b)
				bufputs(&summary, "");
		} else if (!code.n && comment.n && name) {
			bufputc(&summary, ' ');
			bufadd(&summary, comment.s, comment.n);
		}
	}
	if (name) {
		Buf e = {0};
		bufprintf(&e, "**%.*s** \xe2\x80\x94 %s", (int)sig.n, sig.s, summary.b ? summary.b : "");
		strmap_put(kws, name, strlen(name), e.b);
	}
}

static const char *base(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

RefDocs *refdocs_load(const char *dir) {
	if (!dir)
		return NULL;
	RefDocs *d = xcalloc(1, sizeof *d);
	bool found = false;
	for (int i = 0; i < NPages; i++) {
		Buf path = {0};
		bufprintf(&path, "%s/%s", dir, base(refpages[i]));
		FILE *f = fopen(path.b, "rb");
		if (!f)
			continue;
		Buf b = {0};
		char chunk[65536];
		size_t k;
		while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
			bufadd(&b, chunk, k);
		fclose(f);
		found = true;
		Str page = {b.b ? b.b : "", b.n};
		if (strstr(refpages[i], "keywlist.htm"))
			parsekeywordlist(page, &d->keywords);
		else
			parseentries(page, d);
	}
	return found ? d : NULL;
}

int refdocs_fetch(const char *dir) {
	Buf mk = {0};
	bufprintf(&mk, "%s", dir);
	for (char *p = mk.b + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(mk.b, 0755);
			*p = '/';
		}
	mkdir(mk.b, 0755);
	for (int i = 0; i < NPages; i++) {
		Buf url = {0}, out = {0};
		bufprintf(&url, "%s%s", site, refpages[i]);
		bufprintf(&out, "%s/%s", dir, base(refpages[i]));
		fprintf(stderr, "fetching %s\n", url.b);
		pid_t pid = fork();
		if (pid == 0) {
			execlp("curl", "curl", "-fsSL", "--max-time", "30", "--max-filesize", "4194304", "-o", out.b, url.b,
				(char *)NULL);
			fprintf(stderr, "gcon lsp: cannot run curl\n");
			_exit(127);
		}
		int st;
		if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
			fprintf(stderr, "gcon lsp: %s: not fetched\n", url.b);
			return 1;
		}
	}
	RefDocs *d = refdocs_load(dir);
	fprintf(stderr, "gcon lsp: %zu functions and %zu keywords documented in %s\n", d ? d->functions.n : 0,
		d ? d->keywords.n : 0, dir);
	return 0;
}
