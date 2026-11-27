/*
 * gcon doc (doc.h): the documentation tree, the root's doc directory of
 * Markdown pages, shown in the browser as rustup doc shows Rust's, or
 * on standard output in another form. design/design.md, "Delivery".
 *
 * A page's topic is its path in the tree without ".md" ("index",
 * "internals/protocol"). gcon help's pages are pages too, generated:
 * "help/index" (the overview) and "help/NAME".
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "doc.h"

#include "doccomment.h"
#include "driver.h"
#include "help.h"
#include "httpd.h"
#include "markdown.h"
#include "mdrender.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

enum { TextWidth = 72 };

typedef struct Page {
	char *topic;
	char *file; /* the Markdown file, or NULL for a help page */
	int help;   /* the help page's index, for a help page */
} Page;

typedef struct Tree {
	char *dir; /* ROOT/doc */
	VEC(Page) pages;
} Tree;

static int cmptopic(const void *a, const void *b) { return strcmp(((const Page *)a)->topic, ((const Page *)b)->topic); }

static bool hassuffix(const char *s, const char *suf) {
	size_t n = strlen(s), m = strlen(suf);
	return n >= m && !strcmp(s + n - m, suf);
}

/* loadtree lists the pages: the tree's Markdown files (not its html
 * directory, which -build writes) and the help pages. */
static Tree loadtree(const char *dir) {
	Tree t = {0};
	t.dir = (char *)dir;
	VEC(char *) stk = {0};
	VPUSH(stk, "");
	while (stk.n) {
		char *rel = stk.v[--stk.n];
		char *path = *rel ? pathjoin(dir, rel) : (char *)dir;
		DIR *d = opendir(path);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char *r = *rel ? pathjoin(rel, e->d_name) : xstrndup(e->d_name, strlen(e->d_name));
			char *full = pathjoin(dir, r);
			struct stat st;
			if (stat(full, &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode)) {
				if (strcmp(r, "html"))
					VPUSH(stk, r);
			} else if (hassuffix(r, ".md")) {
				Page p = {xstrndup(r, strlen(r) - 3), full, -1};
				VPUSH(t.pages, p);
			}
		}
		closedir(d);
	}
	for (int i = 0; i < help_count(); i++) {
		Buf b = {0};
		const char *n = help_name(i);
		bufprintf(&b, "help/%s", strcmp(n, "gcon") ? n : "index");
		Page p = {b.b, NULL, i};
		VPUSH(t.pages, p);
	}
	qsort(t.pages.v, t.pages.n, sizeof *t.pages.v, cmptopic);
	return t;
}

static Page *findtopic(Tree *t, const char *topic) {
	for (size_t i = 0; i < t->pages.n; i++)
		if (!strcmp(t->pages.v[i].topic, topic))
			return &t->pages.v[i];
	return NULL;
}

static bool readall(const char *path, Buf *b) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(b, chunk, k);
	bool ok = !ferror(f);
	fclose(f);
	if (!b->b)
		bufputs(b, "");
	return ok;
}

/* source is a page's Markdown: its file's, or a help page made into
 * Markdown. */
static char *source(const Page *p, size_t *n) {
	Buf b = {0};
	if (p->file) {
		if (!readall(p->file, &b)) {
			fprintf(stderr, "gcon doc: %s: %s\n", p->file, strerror(errno));
			exit(1);
		}
	} else {
		const char *name = help_name(p->help);
		if (strcmp(name, "gcon"))
			bufprintf(&b, "# gcon help %s\n\n```\n%s```\n", name, help_text(p->help));
		else
			bufprintf(&b, "# gcon help\n\n```\n%s```\n", help_text(p->help));
	}
	*n = b.n;
	return b.b;
}

static bool isref(const char *topic) { return !strncmp(topic, "ref/", 4); }

/* entryname is the name a reference entry's heading gives (its plain
 * text: "find", "&subject"), or NULL if the block is not an entry: a
 * level-2 heading on a reference page that is one word. */
static char *entryname(const Page *p, const MdBlock *b) {
	if (!isref(p->topic) || b->kind != MB_HEADING || b->level != 2)
		return NULL;
	Buf t = {0};
	md_plain(&t, &b->spans);
	if (!t.b || strchr(t.b, ' '))
		return NULL;
	return t.b;
}

/* tier orders the pages a topic's last element may name: the guide and
 * the internals, then help, then the reference. */
static int tier(const char *topic) { return !strncmp(topic, "help/", 5) ? 1 : isref(topic) ? 2 : 0; }

/* A target: the page a name means, and the reference entry in it (its
 * heading's block), or -1 for the whole page. */
typedef struct Target {
	Page *page;
	MdDoc *doc;
	int entry;
} Target;

static MdDoc *parsepage(const Page *p) {
	size_t n;
	char *src = source(p, &n);
	return md_parse(src, n);
}

/* lookup finds what a name means: a topic; else a reference entry
 * ("find", "&subject"); else the page whose topic's last element it is
 * ("protocol" for "internals/protocol"), trying the guide before help
 * and help before the reference. A page of NULL: nothing in the tree. */
static Target lookup(Tree *t, const char *name) {
	char *nm = xstrndup(name, strlen(name));
	if (hassuffix(nm, ".md"))
		nm[strlen(nm) - 3] = 0;
	while (!strncmp(nm, "./", 2))
		nm += 2;
	if (!*nm)
		nm = "index";
	Target r = {findtopic(t, nm), NULL, -1};
	if (r.page)
		return r;
	int count = 0;
	for (size_t i = 0; i < t->pages.n; i++) {
		Page *p = &t->pages.v[i];
		if (!isref(p->topic))
			continue;
		MdDoc *d = parsepage(p);
		for (size_t bi = 0; bi < d->blocks.n; bi++) {
			char *en = entryname(p, &d->blocks.v[bi]);
			if (en && !strcmp(en, nm)) {
				if (count++)
					fprintf(stderr, "gcon doc %s: an entry in both %s and %s\n", name, r.page->topic, p->topic);
				r = (Target){p, d, (int)bi};
			}
		}
	}
	if (count == 1)
		return r;
	if (count > 1)
		exit(2);
	for (int tr = 0; tr < 3; tr++) {
		count = 0;
		for (size_t i = 0; i < t->pages.n; i++) {
			const char *tp = t->pages.v[i].topic;
			const char *slash = strrchr(tp, '/');
			if (tier(tp) == tr && !strcmp(slash ? slash + 1 : tp, nm)) {
				r.page = &t->pages.v[i];
				count++;
			}
		}
		if (count == 1)
			return r;
		if (count > 1) {
			fprintf(stderr, "gcon doc %s: more than one page:", name);
			for (size_t i = 0; i < t->pages.n; i++) {
				const char *tp = t->pages.v[i].topic;
				const char *slash = strrchr(tp, '/');
				if (tier(tp) == tr && !strcmp(slash ? slash + 1 : tp, nm))
					fprintf(stderr, " %s", tp);
			}
			fputc('\n', stderr);
			exit(2);
		}
	}
	return (Target){NULL, NULL, -1}; /* nothing in the tree: the caller tries the IPL */
}

/* entryend is the block after entry e's last: the next heading of its
 * level or above. */
static size_t entryend(const MdDoc *d, int e) {
	int level = d->blocks.v[e].level;
	size_t j = (size_t)e + 1;
	while (j < d->blocks.n && !(d->blocks.v[j].kind == MB_HEADING && d->blocks.v[j].level <= level))
		j++;
	return j;
}

/* entrydoc is entry e of d as a page of its own. */
static MdDoc *entrydoc(const MdDoc *d, int e) {
	MdDoc *s = xcalloc(1, sizeof *s);
	size_t end = entryend(d, e);
	for (size_t i = (size_t)e; i < end; i++)
		VPUSH(s->blocks, d->blocks.v[i]);
	s->blocks.v[0].level = 1; /* the entry's title, as a page's */
	Buf t = {0};
	md_plain(&t, &d->blocks.v[e].spans);
	s->title = t.b ? t.b : "";
	return s;
}

/* entrysource is entry e's Markdown, from src: from its heading's line
 * (or the line after, without it) to the line before the next entry,
 * trailing blank lines dropped. */
static char *entrysource(const char *src, const MdDoc *d, int e, bool heading) {
	size_t end = entryend(d, e);
	int from = d->blocks.v[e].line + (heading ? 0 : 1);
	int to = end < d->blocks.n ? d->blocks.v[end].line : 1 << 30; /* exclusive */
	Buf b = {0};
	int line = 1;
	for (const char *s = src; *s && line < to;) {
		const char *nl = strchr(s, '\n');
		size_t n = nl ? (size_t)(nl - s) + 1 : strlen(s);
		if (line >= from)
			bufadd(&b, s, n);
		s += n;
		line++;
	}
	while (b.n && (b.b[b.n - 1] == '\n' || b.b[b.n - 1] == ' '))
		b.b[--b.n] = 0;
	while (b.n && b.b[0] == '\n')
		memmove(b.b, b.b + 1, b.n--);
	if (b.b)
		bufputc(&b, '\n');
	return b.b ? b.b : "";
}

struct DocRef {
	StrMap entries; /* name -> its Markdown */
};

/* noexamples is an entry's Markdown without its examples (an icon code
 * block, and an "Output:" paragraph and the code block after it): a
 * hover shows what a function is, not a program using it. */
static char *noexamples(const char *md) {
	Buf b = {0};
	enum { Text, Example, AfterExample, Output } st = Text;
	int fences = 0; /* of the output's block, in state Output */
	for (const char *s = md; *s;) {
		const char *nl = strchr(s, '\n');
		size_t n = nl ? (size_t)(nl - s) + 1 : strlen(s);
		bool fence = !strncmp(s, "```", 3);
		bool blank = n == 1 && *s == '\n';
		switch (st) {
		case Text:
			if (fence && !strncmp(s, "```icon", 7))
				st = Example;
			else
				bufadd(&b, s, n);
			break;
		case Example:
			if (fence)
				st = AfterExample;
			break;
		case AfterExample:
			if (!strncmp(s, "Output:", 7)) {
				st = Output;
				fences = 0;
			} else if (!blank) {
				st = Text;
				bufadd(&b, s, n);
			}
			break;
		case Output:
			/* through the output block's closing fence */
			if (fence && ++fences == 2)
				st = Text;
			break;
		}
		s += n;
	}
	/* blank lines left where examples were: at most one in a row */
	Buf c = {0};
	for (size_t i = 0; i < b.n; i++) {
		if (b.b[i] == '\n' && c.n >= 2 && c.b[c.n - 1] == '\n' && c.b[c.n - 2] == '\n')
			continue;
		bufputc(&c, b.b[i]);
	}
	while (c.n && c.b[c.n - 1] == '\n') /* the hover adds its own spacing */
		c.b[--c.n] = 0;
	return c.b ? c.b : "";
}

DocRef *docref_load(const char *root) {
	if (!root)
		return NULL;
	Tree t = loadtree(pathjoin(root, "doc"));
	DocRef *r = NULL;
	for (size_t i = 0; i < t.pages.n; i++) {
		Page *p = &t.pages.v[i];
		if (!isref(p->topic))
			continue;
		size_t n;
		char *src = source(p, &n);
		MdDoc *d = md_parse(src, n);
		for (size_t bi = 0; bi < d->blocks.n; bi++) {
			char *en = entryname(p, &d->blocks.v[bi]);
			if (!en)
				continue;
			if (!r)
				r = xcalloc(1, sizeof *r);
			strmap_put(&r->entries, en, strlen(en), noexamples(entrysource(src, d, (int)bi, false)));
		}
	}
	return r;
}

const char *docref_entry(DocRef *r, const char *name) { return r ? strmap_get(&r->entries, name, strlen(name)) : NULL; }

/* ------------------------------------------------------------------ */
/* Examples */

/* An example: an icon code block, and the output it must give if an
 * "Output:" paragraph and a code block follow it. */
typedef struct Example {
	const char *code;
	const char *want; /* NULL: it need only translate */
	int line;
} Example;

typedef VEC(Example) Examples;

static bool isoutput(const MdBlock *b) {
	if (b->kind != MB_PARA)
		return false;
	Buf t = {0};
	md_plain(&t, &b->spans);
	return t.b && !strcmp(t.b, "Output:");
}

/* examples lists the examples of blocks [from, to) of d: icon code
 * blocks that are whole programs (they declare main). */
static Examples examples(const MdDoc *d, size_t from, size_t to) {
	Examples v = {0};
	for (size_t i = from; i < to; i++) {
		const MdBlock *b = &d->blocks.v[i];
		if (b->kind != MB_CODE || !b->lang || strcmp(b->lang, "icon") || !strstr(b->code, "procedure main"))
			continue;
		Example e = {b->code, NULL, b->line};
		if (i + 2 < to && isoutput(&d->blocks.v[i + 1]) && d->blocks.v[i + 2].kind == MB_CODE)
			e.want = d->blocks.v[i + 2].code;
		VPUSH(v, e);
	}
	return v;
}

/* runself runs gcon with argv (argv[0] ignored), standard input empty,
 * and returns its standard output and error together, and its status. */
static char *runself(char **argv, int *status) {
	int fds[2];
	if (pipe(fds) != 0)
		return NULL;
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		return NULL;
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[1]);
		int in = open("/dev/null", O_RDONLY);
		if (in >= 0)
			dup2(in, 0);
		execv(executable("gcon"), argv);
		_exit(127);
	}
	close(fds[1]);
	Buf b = {0};
	char chunk[4096];
	ssize_t k;
	while ((k = read(fds[0], chunk, sizeof chunk)) > 0 || (k < 0 && errno == EINTR))
		if (k > 0)
			bufadd(&b, chunk, (size_t)k);
	close(fds[0]);
	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	*status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
	return b.b ? b.b : xstrndup("", 0);
}

static void indented(const char *label, const char *s) {
	fprintf(stderr, "\t%s:\n", label);
	for (; *s;) {
		const char *nl = strchr(s, '\n');
		size_t n = nl ? (size_t)(nl - s) : strlen(s);
		fprintf(stderr, "\t\t%.*s\n", (int)n, s);
		s += n + (nl ? 1 : 0);
	}
}

/* testexamples runs the examples of the pages (or of one target): each
 * with an output must give it, under gcon run; each without must
 * translate (gcon dis). Returns the number that failed. */
static int testexamples(Tree *t, Target *only, int *nrun, int *ntrans) {
	const char *tmpdir = getenv("TMPDIR");
	Buf tmpl = {0};
	bufprintf(&tmpl, "%s/gcondocXXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
	char *d0 = mkdtemp(tmpl.b);
	if (!d0) {
		fprintf(stderr, "gcon doc: %s: %s\n", tmpl.b, strerror(errno));
		return 1;
	}
	char *file = pathjoin(d0, "example.icn");
	int bad = 0;
	for (size_t i = 0; i < t->pages.n; i++) {
		Page *p = &t->pages.v[i];
		if (only && only->page != p)
			continue;
		MdDoc *d = only && only->doc ? only->doc : parsepage(p);
		size_t from = 0, to = d->blocks.n;
		if (only && only->entry >= 0)
			from = (size_t)only->entry, to = entryend(d, only->entry);
		Examples ex = examples(d, from, to);
		for (size_t k = 0; k < ex.n; k++) {
			const Example *e = &ex.v[k];
			FILE *f = fopen(file, "w");
			if (!f || fputs(e->code, f) == EOF || fclose(f) != 0) {
				fprintf(stderr, "gcon doc: write %s: %s\n", file, strerror(errno));
				return bad + 1;
			}
			char *argv[] = {"gcon", e->want ? "run" : "dis", file, NULL};
			int st;
			char *got = runself(argv, &st);
			if (e->want) {
				(*nrun)++;
				if (!got || strcmp(got, e->want)) {
					fprintf(stderr, "%s.md:%d: the example's output is not what the page says\n", p->topic, e->line);
					indented("want", e->want);
					indented("got", got ? got : "");
					bad++;
				}
			} else {
				(*ntrans)++;
				if (st != 0) {
					fprintf(stderr, "%s.md:%d: the example does not translate\n", p->topic, e->line);
					indented("gcon dis", got ? got : "");
					bad++;
				}
			}
		}
	}
	unlink(file);
	rmdir(d0);
	return bad;
}

/* ------------------------------------------------------------------ */
/* Checking */

/* resolve gives the topic and anchor a link from page topic means, or
 * NULL topic for a link that is not to a page in the tree. */
static char *resolve(const char *from, const char *target, char **anchor) {
	const char *hash = strchr(target, '#');
	*anchor = hash ? xstrndup(hash + 1, strlen(hash + 1)) : NULL;
	size_t pn = hash ? (size_t)(hash - target) : strlen(target);
	if (pn == 0)
		return xstrndup(from, strlen(from));
	if (pn < 3 || strncmp(target + pn - 3, ".md", 3))
		return NULL;
	/* the link's path, relative to from's directory, made canonical */
	Buf b = {0};
	const char *slash = strrchr(from, '/');
	if (slash && target[0] != '/')
		bufadd(&b, from, (size_t)(slash - from) + 1);
	bufadd(&b, target, pn - 3);
	VEC(char *) parts = {0};
	char *s = b.b;
	while (s && *s) {
		char *e = strchr(s, '/');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		if (n == 2 && !strncmp(s, "..", 2)) {
			if (!parts.n)
				return NULL;
			parts.n--;
		} else if (n && !(n == 1 && *s == '.'))
			VPUSH(parts, xstrndup(s, n));
		s = e ? e + 1 : NULL;
	}
	Buf t = {0};
	for (size_t i = 0; i < parts.n; i++) {
		if (i)
			bufputc(&t, '/');
		bufputs(&t, parts.v[i]);
	}
	return t.b ? t.b : "";
}

static bool hasanchor(const MdDoc *d, const char *a) {
	for (size_t i = 0; i < d->blocks.n; i++)
		if (d->blocks.v[i].kind == MB_HEADING && !strcmp(d->blocks.v[i].anchor, a))
			return true;
	return false;
}

/* checklinks reports the links of spans that lead nowhere. */
static int checklinks(Tree *t, MdDoc **docs, const Page *p, const MdBlock *b, const MdSpans *s) {
	int bad = 0;
	for (size_t i = 0; i < s->n; i++) {
		const MdSpan *sp = &s->v[i];
		if (sp->kind != MS_LINK || sp->end || md_external(sp->s))
			continue;
		char *anchor;
		char *topic = resolve(p->topic, sp->s, &anchor);
		Page *q = topic ? findtopic(t, topic) : NULL;
		if (!q) {
			fprintf(stderr, "%s.md:%d: a link to %s, which is not a page of the tree\n", p->topic, b->line, sp->s);
			bad++;
		} else if (anchor && !hasanchor(docs[q - t->pages.v], anchor)) {
			fprintf(stderr, "%s.md:%d: a link to %s, but %s has no heading #%s\n", p->topic, b->line, sp->s, q->topic, anchor);
			bad++;
		}
	}
	return bad;
}

/* parseall parses every page, reporting Markdown that is not the
 * documentation's, and returns the problems found. */
static int parseall(Tree *t, MdDoc ***out) {
	MdDoc **docs = xcalloc(t->pages.n, sizeof *docs);
	int bad = 0;
	for (size_t i = 0; i < t->pages.n; i++) {
		size_t n;
		char *src = source(&t->pages.v[i], &n);
		docs[i] = md_parse(src, n);
		for (size_t k = 0; k < docs[i]->errs.n; k++) {
			fprintf(stderr, "%s.md: %s\n", t->pages.v[i].topic, docs[i]->errs.v[k]);
			bad++;
		}
		if (!*docs[i]->title) {
			fprintf(stderr, "%s.md: no heading to title it\n", t->pages.v[i].topic);
			bad++;
		}
	}
	*out = docs;
	return bad;
}

static int checkall(Tree *t, MdDoc **docs) {
	int bad = 0;
	for (size_t i = 0; i < t->pages.n; i++) {
		const MdDoc *d = docs[i];
		for (size_t bi = 0; bi < d->blocks.n; bi++) {
			const MdBlock *b = &d->blocks.v[bi];
			bad += checklinks(t, docs, &t->pages.v[i], b, &b->spans);
			for (int c = 0; c < b->nrows * b->ncols; c++)
				bad += checklinks(t, docs, &t->pages.v[i], b, &b->cells[c]);
		}
	}
	return bad;
}

/* ------------------------------------------------------------------ */
/* HTML pages */

static const char style[] =
#include "docstyle.inc"
	;

static const char script[] =
#include "docsearch.inc"
	;

/* relroot is the way from topic's directory up to the tree's top. */
static char *relroot(const char *topic) {
	Buf b = {0};
	for (const char *s = topic; *s; s++)
		if (*s == '/')
			bufputs(&b, "../");
	return b.b ? b.b : "";
}

/* runtoken is the local server's secret, given to its pages so that
 * their examples may be run (POST /run); NULL in the static HTML. */
static const char *runtoken;

static void htmlpage(Buf *out, const Page *p, const MdDoc *d) {
	char *up = relroot(p->topic);
	bufputs(out, "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
		     "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
	md_htmlesc(out, d->title, strlen(d->title));
	bufputs(out, " \xe2\x80\x94 Gcon</title>\n");
	bufprintf(out, "<link rel=\"stylesheet\" href=\"%sstyle.css\">\n", up);
	bufprintf(out, "</head>\n<body data-root=\"%s\"", up);
	if (runtoken)
		bufprintf(out, " data-run=\"%s\"", runtoken);
	bufputs(out, ">\n");
	bufprintf(out, "<header><a class=\"home\" href=\"%sindex.html\">Gcon documentation</a>"
		       "<span class=\"version\">%s</span>"
		       "<input id=\"search\" type=\"search\" placeholder=\"Search\" aria-label=\"Search the documentation\" autocomplete=\"off\">"
		       "<ul id=\"results\" hidden></ul></header>\n<main>\n",
		up, GCON_VERSION);
	md_html(out, d);
	bufprintf(out, "</main>\n<script src=\"%ssearch-index.js\"></script>\n<script src=\"%ssearch.js\"></script>\n</body>\n</html>\n", up, up);
}

static bool writefile(const char *path, const char *s, size_t n) {
	/* make the directories */
	char *dir = xstrndup(path, strlen(path));
	for (char *q = dir + 1; *q; q++) {
		if (*q == '/') {
			*q = 0;
			mkdir(dir, 0777);
			*q = '/';
		}
	}
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(s, 1, n, f) != n || fclose(f) != 0) {
		fprintf(stderr, "gcon doc: write %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

/* jsstr appends s as a JavaScript string literal. */
static void jsstr(Buf *out, const char *s) {
	bufputc(out, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			bufputc(out, '\\');
		if (*s == '<')
			bufputs(out, "\\u003c");
		else if ((unsigned char)*s < ' ')
			bufputc(out, ' ');
		else
			bufputc(out, *s);
	}
	bufputc(out, '"');
}

/* searchindex is the search's index, search-index.js: every heading of
 * every page. */
static void searchindex(Buf *idx, Tree *t, MdDoc **docs) {
	bufputs(idx, "var GCONDOC = [\n");
	for (size_t i = 0; i < t->pages.n; i++) {
		const Page *p = &t->pages.v[i];
		for (size_t bi = 0; bi < docs[i]->blocks.n; bi++) {
			const MdBlock *h = &docs[i]->blocks.v[bi];
			if (h->kind != MB_HEADING)
				continue;
			Buf plain = {0};
			md_plain(&plain, &h->spans);
			Buf url = {0};
			bufprintf(&url, "%s.html", p->topic);
			if (bi > 0 || h->level > 1)
				bufprintf(&url, "#%s", h->anchor);
			bufputs(idx, "[");
			jsstr(idx, plain.b ? plain.b : "");
			bufputs(idx, ",");
			jsstr(idx, url.b);
			bufputs(idx, ",");
			jsstr(idx, docs[i]->title);
			bufputs(idx, "],\n");
		}
	}
	bufputs(idx, "];\n");
}

static int build(Tree *t, MdDoc **docs, const char *outdir) {
	for (size_t i = 0; i < t->pages.n; i++) {
		const Page *p = &t->pages.v[i];
		Buf b = {0};
		htmlpage(&b, p, docs[i]);
		Buf path = {0};
		bufprintf(&path, "%s/%s.html", outdir, p->topic);
		if (!writefile(path.b, b.b, b.n))
			return 1;
	}
	Buf idx = {0};
	searchindex(&idx, t, docs);
	Buf p1 = {0}, p2 = {0}, p3 = {0};
	bufprintf(&p1, "%s/style.css", outdir);
	bufprintf(&p2, "%s/search.js", outdir);
	bufprintf(&p3, "%s/search-index.js", outdir);
	if (!writefile(p1.b, style, strlen(style)) || !writefile(p2.b, script, strlen(script)) || !writefile(p3.b, idx.b, idx.n))
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* The Icon Program Library */

/* The IPL is shipped for compatibility, not documented by Gcon: its
 * files' header comments are shown as Arizona wrote them, apart from
 * the tree (not in the HTML, the search or -list). design/design.md,
 * "Documentation". */
static const char *const ipldirs[] = {"lib/ipl/procs", "lib/ipl/gprocs"};
enum { NIplDirs = sizeof ipldirs / sizeof ipldirs[0] };

static const char iplnote[] =
	"From the Icon Program Library, as Arizona wrote it (it is in the public "
	"domain). It is not part of Gcon's reference: Gcon ships the library so "
	"that Icon programs run unchanged.";

/* iplheader is a file's header comment: its leading comment lines, the
 * # taken off, tabs expanded as in the file (the # in the first column),
 * a rule of #s as a blank line; and each section between rules dedented
 * by its least indentation, so that a tab-indented section and a
 * space-indented one both start at the margin, their own layout kept. */
static char *iplheader(const char *src) {
	VEC(char *) lines = {0};
	for (const char *s = src; *s == '#';) {
		const char *nl = strchr(s, '\n');
		size_t n = nl ? (size_t)(nl - s) : strlen(s);
		size_t k = 0;
		while (k < n && s[k] == '#')
			k++;
		if (k >= 10)
			VPUSH(lines, NULL); /* a rule */
		else {
			Buf b = {0};
			int col = 1;
			for (size_t i = 1; i < n; i++) {
				if (s[i] == '\t') {
					do
						bufputc(&b, ' '), col++;
					while (col % 8);
				} else if (s[i] != '\r')
					bufputc(&b, s[i]), col++;
			}
			while (b.n && b.b[b.n - 1] == ' ')
				b.b[--b.n] = 0;
			VPUSH(lines, b.b ? b.b : xstrndup("", 0));
		}
		s += n + (nl ? 1 : 0);
	}
	/* dedent each section */
	for (size_t i = 0; i < lines.n;) {
		size_t j = i;
		size_t min = (size_t)-1;
		for (; j < lines.n && lines.v[j]; j++)
			if (*lines.v[j] && strspn(lines.v[j], " ") < min)
				min = strspn(lines.v[j], " ");
		for (size_t k = i; k < j; k++)
			if (*lines.v[k])
				lines.v[k] += min;
		i = j + 1;
	}
	/* rules and empty lines as blank lines: none at the ends, at most
	 * one in a row */
	Buf c = {0};
	bool blank = true;
	for (size_t i = 0; i < lines.n; i++) {
		if (!lines.v[i] || !*lines.v[i]) {
			if (!blank)
				bufputc(&c, '\n');
			blank = true;
			continue;
		}
		bufputs(&c, lines.v[i]);
		bufputc(&c, '\n');
		blank = false;
	}
	while (c.n >= 2 && c.b[c.n - 1] == '\n' && c.b[c.n - 2] == '\n')
		c.b[--c.n] = 0;
	return c.b ? c.b : "";
}

/* iplprocs lists a file's procedure headings, with their "#:" summaries. */
static char *iplprocs(const char *src) {
	Buf b = {0};
	for (const char *s = src; *s;) {
		const char *nl = strchr(s, '\n');
		size_t n = nl ? (size_t)(nl - s) : strlen(s);
		if (n > 10 && !strncmp(s, "procedure ", 10)) {
			bufadd(&b, s, n);
			while (b.n && (b.b[b.n - 1] == ' ' || b.b[b.n - 1] == '\t' || b.b[b.n - 1] == '\r'))
				b.b[--b.n] = 0;
			bufputc(&b, '\n');
		}
		s += n + (nl ? 1 : 0);
	}
	return b.b;
}

static char *subject(const char *src) {
	const char *s = strstr(src, "Subject:");
	if (!s || s - src > 2000)
		return "";
	s += 8;
	while (*s == ' ' || *s == '\t')
		s++;
	const char *e = strchr(s, '\n');
	return xstrndup(s, e ? (size_t)(e - s) : strlen(s));
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef VEC(char *) StrList;

/* iplfiles lists the IPL's files, "procs/strings", sorted. */
static StrList iplfiles(const char *root) {
	StrList v = {0};
	for (int i = 0; i < NIplDirs; i++) {
		DIR *d = opendir(pathjoin(root, ipldirs[i]));
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d)))
			if (hassuffix(e->d_name, ".icn")) {
				Buf f = {0};
				bufprintf(&f, "%s/%.*s", ipldirs[i] + 8, (int)strlen(e->d_name) - 4, e->d_name);
				VPUSH(v, f.b);
			}
		closedir(d);
	}
	qsort(v.v, v.n, sizeof *v.v, cmpstr);
	return v;
}

/* candidate reports whether the IPL file ("procs/strings") is on the
 * list of candidates for Gcon's standard library, doc/ipl-candidates.txt
 * (a file per line; # begins a comment). */
static bool candidate(const char *root, const char *file) {
	static StrMap list;
	static bool loaded;
	if (!loaded) {
		loaded = true;
		Buf b = {0};
		if (readall(pathjoin(root, "doc/ipl-candidates.txt"), &b))
			for (char *s = b.b; *s;) {
				char *nl = strchr(s, '\n');
				size_t n = nl ? (size_t)(nl - s) : strlen(s);
				char *h = memchr(s, '#', n);
				size_t m = h ? (size_t)(h - s) : n;
				while (m && (s[m - 1] == ' ' || s[m - 1] == '\t' || s[m - 1] == '\r'))
					m--;
				if (m)
					strmap_put(&list, s, m, (void *)1);
				s += n + (nl ? 1 : 0);
			}
	}
	return strmap_get(&list, file, strlen(file)) != NULL;
}

/* The banner on a candidate's page, as rustdoc marks nightly or
 * deprecated items. */
static const char candidatebanner[] = "**\\[candidate for Gcon's standard library\\]**\n\n";

static char *iplsource(const char *root, const char *file) {
	Buf p = {0}, b = {0};
	bufprintf(&p, "%s/lib/ipl/%s.icn", root, file);
	return readall(p.b, &b) ? b.b : NULL;
}

/* iplmd is the page for name, an IPL file ("strings") or procedure
 * ("WOpen"), as Markdown; NULL if it is neither. */
static char *iplmd(const char *root, const char *name) {
	char *nm = xstrndup(name, strlen(name));
	if (!strncmp(nm, "ipl/", 4))
		nm += 4;
	if (hassuffix(nm, ".icn"))
		nm[strlen(nm) - 4] = 0;
	const char *slash = strrchr(nm, '/');
	const char *base = slash ? slash + 1 : nm;
	StrList files = iplfiles(root);
	Buf md = {0};
	for (size_t i = 0; i < files.n; i++) {
		const char *fb = strrchr(files.v[i], '/') + 1;
		if (strcmp(fb, base) && strcmp(files.v[i], nm))
			continue;
		char *src = iplsource(root, files.v[i]);
		if (!src)
			continue;
		bufprintf(&md, "# ipl/%s\n\n%s%s\n\n```\n%s```\n", files.v[i],
			candidate(root, files.v[i]) ? candidatebanner : "", iplnote, iplheader(src));
		char *procs = iplprocs(src);
		if (procs)
			bufprintf(&md, "\nIts procedures (`link %s`):\n\n```\n%s```\n", fb, procs);
		return md.b;
	}
	/* a procedure: the files that declare it */
	Buf head = {0};
	bufprintf(&head, "procedure %s(", base);
	int found = 0;
	Buf also = {0};
	for (size_t i = 0; i < files.n; i++) {
		char *src = iplsource(root, files.v[i]);
		if (!src)
			continue;
		for (const char *s = src; (s = strstr(s, head.b));) {
			if (s != src && s[-1] != '\n') {
				s++;
				continue;
			}
			const char *nl = strchr(s, '\n');
			char *decl = xstrndup(s, nl ? (size_t)(nl - s) : strlen(s));
			while (*decl && (decl[strlen(decl) - 1] == ' ' || decl[strlen(decl) - 1] == '\t' || decl[strlen(decl) - 1] == '\r'))
				decl[strlen(decl) - 1] = 0;
			const char *fb = strrchr(files.v[i], '/') + 1;
			if (found++)
				bufprintf(&also, "%s`ipl/%s`", also.n ? ", " : "", files.v[i]);
			else
				bufprintf(&md, "# %s\n\n%s%s\n\n```\n%s\n```\n\nIn `ipl/%s` (`link %s`), whose header says:\n\n```\n%s```\n",
					base, candidate(root, files.v[i]) ? candidatebanner : "", iplnote, decl, files.v[i], fb, iplheader(src));
			break;
		}
	}
	if (also.n)
		bufprintf(&md, "\nAlso declared in %s.\n", also.b);
	return found ? md.b : NULL;
}

/* ipllist writes the IPL's files and their subjects. */
static void ipllist(const char *root) {
	StrList files = iplfiles(root);
	printf("The Icon Program Library, as Arizona wrote it: gcon doc ipl/NAME shows a file or\nprocedure. \"candidate\": a candidate for Gcon's standard library (doc/ipl-candidates.txt).\n\n");
	for (size_t i = 0; i < files.n; i++) {
		char *src = iplsource(root, files.v[i]);
		printf("%-24s %-10s %s\n", files.v[i], candidate(root, files.v[i]) ? "candidate" : "", src ? subject(src) : "");
	}
}

/* ------------------------------------------------------------------ */
/* Showing a page */

/* pager writes text through $PAGER (default less) when standard output
 * is a terminal. */
static void pager(const char *text) {
	if (isatty(1)) {
		const char *pg = getenv("PAGER");
		if (!pg)
			pg = "less";
		if (*pg) {
			fflush(stdout);
			FILE *f = popen(pg, "w");
			if (f) {
				fputs(text, f);
				if (pclose(f) != -1)
					return;
			}
		}
	}
	fputs(text, stdout);
}

/* browse opens url in the browser: $BROWSER, else open on macOS, else
 * xdg-open where there is a display. False if there is none to open or
 * it fails. */
static bool browse(const char *url) {
	const char *b = getenv("BROWSER");
	const char *cmd;
	if (b && *b)
		cmd = b;
	else {
#if defined(__APPLE__)
		cmd = "open";
#else
		const char *x = getenv("DISPLAY"), *w = getenv("WAYLAND_DISPLAY");
		if (!(x && *x) && !(w && *w))
			return false;
		cmd = "xdg-open";
#endif
	}
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		return false;
	if (pid == 0) {
		/* $BROWSER may carry arguments: sh splits it */
		execl("/bin/sh", "sh", "-c", "exec $0 \"$1\" >/dev/null 2>&1", cmd, url, (char *)NULL);
		_exit(127);
	}
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return false;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ------------------------------------------------------------------ */
/* The local server */

/* By default gcon doc serves the documentation itself, on a port of
 * the loopback address, and opens the browser at it, as go doc -http
 * does: so the IPL and a program's own doc comments are pages too, and
 * the examples can be edited and run (POST /run, which gcon run runs).
 * A run needs the server's secret, which only its own pages carry, and
 * a Host of the loopback address (against DNS rebinding); served
 * beyond the machine (-http with another address), nothing runs. */

enum {
	RunLimit = 60,      /* seconds a run may take */
	RunOutput = 1 << 20, /* bytes of its output kept */
};

typedef struct Site {
	const char *root; /* GCONROOT, or NULL */
	Tree *t;          /* the tree, or NULL */
	MdDoc **docs;
	Buf idx;          /* search-index.js */
	const char *extra; /* a page of the user's own ("/file/x.html"), or NULL */
	Buf extrahtml;
	VEC(char *) hosts; /* the Host values accepted; none: any */
} Site;

/* mdesc appends s with Markdown's punctuation escaped. */
static void mdesc(Buf *b, const char *s) {
	for (; *s; s++) {
		if (strchr("\\`*_[]<>|#", *s))
			bufputc(b, '\\');
		bufputc(b, *s);
	}
}

/* iplindexmd is the IPL's index as a page: gcon doc ipl, with links. */
static char *iplindexmd(const char *root) {
	StrList files = iplfiles(root);
	Buf b = {0};
	bufprintf(&b, "# The Icon Program Library\n\n%s A file marked as a candidate is a candidate for Gcon's standard library.\n\n", iplnote);
	for (size_t i = 0; i < files.n; i++) {
		char *src = iplsource(root, files.v[i]);
		bufprintf(&b, "- [%s](%s.md)%s", files.v[i], files.v[i], candidate(root, files.v[i]) ? " (candidate)" : "");
		const char *subj = src ? subject(src) : "";
		if (*subj) {
			bufputs(&b, ": ");
			mdesc(&b, subj);
		}
		bufputc(&b, '\n');
	}
	return b.b;
}

static void replyhtml(HttpReq *r, int status, const char *topic, const char *md) {
	MdDoc *d = md_parse(md, strlen(md));
	Page p = {(char *)topic, NULL, -1};
	Buf out = {0};
	htmlpage(&out, &p, d);
	http_reply(r, status, "text/html; charset=utf-8", NULL, out.b, out.n);
}

/* rmtree removes a directory and everything in it. */
static void rmtree(const char *dir) {
	DIR *d = opendir(dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char *p = pathjoin(dir, e->d_name);
			struct stat st;
			if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
				rmtree(p);
			else
				unlink(p);
		}
		closedir(d);
	}
	rmdir(dir);
}

/* runreq runs the program in the request's body, as prog.icn in a
 * directory of its own, under gcon run, and replies with its output
 * (standard output and error together) and, in X-Gcon-Status, its exit
 * status. If the browser gives up (the Stop button aborts the request)
 * or the time limit passes, the program and its children are killed. */
static void runreq(HttpReq *r) {
	if (strcmp(r->method, "POST")) {
		http_reply(r, 405, "text/plain", NULL, "POST only\n", 10);
		return;
	}
	const char *tok = http_header(r, "x-gcon-token");
	if (!runtoken || !tok || strcmp(tok, runtoken)) {
		static const char no[] = "running examples is only for the server's own pages, on this machine\n";
		http_reply(r, 403, "text/plain", NULL, no, sizeof no - 1);
		return;
	}
	const char *tmpdir = getenv("TMPDIR");
	Buf tmpl = {0};
	bufprintf(&tmpl, "%s/gcondocXXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
	char *dir = mkdtemp(tmpl.b);
	char *file = dir ? pathjoin(dir, "prog.icn") : NULL;
	FILE *f = file ? fopen(file, "w") : NULL;
	if (!f || fwrite(r->body, 1, r->nbody, f) != r->nbody || fclose(f) != 0) {
		Buf m = {0};
		bufprintf(&m, "gcon doc: cannot write the program: %s\n", strerror(errno));
		http_reply(r, 500, "text/plain", NULL, m.b, m.n);
		return;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		rmtree(dir);
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0); /* a group of its own, to kill it whole */
		close(fds[0]);
		close(r->fd);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[1]);
		int in = open("/dev/null", O_RDONLY);
		if (in >= 0)
			dup2(in, 0);
		if (chdir(dir) != 0)
			_exit(127);
		execl(executable("gcon"), "gcon", "run", "prog.icn", (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		rmtree(dir);
		return;
	}
	setpgid(pid, pid);
	Buf out = {0};
	bool killed = false, gone = false, overflow = false, eof = false;
	int st = 0;
	bool exited = false;
	struct timeval t0, now;
	gettimeofday(&t0, NULL);
	while (!eof || !exited) {
		if (!exited && waitpid(pid, &st, WNOHANG) == pid)
			exited = true;
		if (exited && eof)
			break;
		gettimeofday(&now, NULL);
		long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_usec - t0.tv_usec) / 1000;
		if (ms > RunLimit * 1000 && !killed) {
			kill(-pid, SIGKILL);
			killed = true;
		}
		struct pollfd pf[2] = {{r->fd, POLLIN, 0}, {fds[0], POLLIN, 0}};
		int n = poll(pf, eof ? 1 : 2, 200);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && !eof && pf[1].revents) {
			char chunk[8192];
			ssize_t k = read(fds[0], chunk, sizeof chunk);
			if (k > 0) {
				size_t room = out.n < RunOutput ? RunOutput - out.n : 0;
				bufadd(&out, chunk, (size_t)k < room ? (size_t)k : room);
				overflow = overflow || (size_t)k > room;
			} else if (k == 0 || errno != EINTR)
				eof = true;
		}
		if (n > 0 && pf[0].revents && !gone) {
			char c;
			if (recv(r->fd, &c, 1, MSG_PEEK) <= 0) {
				gone = true; /* the browser gave up: Stop */
				kill(-pid, SIGKILL);
			}
		}
		if (exited && !eof) {
			/* the program is done, but something it started holds the
			 * pipe open: take what is there and finish */
			struct pollfd q = {fds[0], POLLIN, 0};
			while (poll(&q, 1, 50) > 0) {
				char chunk[8192];
				ssize_t k = read(fds[0], chunk, sizeof chunk);
				if (k <= 0)
					break;
				size_t room = out.n < RunOutput ? RunOutput - out.n : 0;
				bufadd(&out, chunk, (size_t)k < room ? (size_t)k : room);
			}
			eof = true;
		}
	}
	close(fds[0]);
	kill(-pid, SIGKILL); /* anything it left behind */
	if (!exited)
		waitpid(pid, &st, 0);
	rmtree(dir);
	if (gone)
		return;
	if (overflow)
		bufprintf(&out, "\n[output cut at %d bytes]\n", RunOutput);
	if (killed)
		bufprintf(&out, "\n[killed: it ran for more than %d seconds]\n", RunLimit);
	int status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	Buf hdr = {0};
	bufprintf(&hdr, "X-Gcon-Status: %d\r\n", status);
	http_reply(r, 200, "text/plain; charset=utf-8", hdr.b, out.b ? out.b : "", out.n);
}

static void serve(HttpReq *r, void *arg) {
	Site *s = arg;
	if (s->hosts.n) {
		const char *host = http_header(r, "host");
		bool ok = false;
		for (size_t i = 0; host && i < s->hosts.n; i++)
			ok = ok || !strcmp(host, s->hosts.v[i]);
		if (!ok) {
			http_reply(r, 403, "text/plain", NULL, "unknown host\n", 13);
			return;
		}
	}
	if (!strcmp(r->path, "/run")) {
		runreq(r);
		return;
	}
	if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
		http_reply(r, 405, "text/plain", NULL, "GET only\n", 9);
		return;
	}
	const char *path = r->path;
	if (!strcmp(path, "/style.css")) {
		http_reply(r, 200, "text/css; charset=utf-8", NULL, style, strlen(style));
		return;
	}
	if (!strcmp(path, "/search.js")) {
		http_reply(r, 200, "text/javascript; charset=utf-8", NULL, script, strlen(script));
		return;
	}
	if (!strcmp(path, "/search-index.js")) {
		http_reply(r, 200, "text/javascript; charset=utf-8", NULL, s->idx.b ? s->idx.b : "var GCONDOC = [];\n", s->idx.b ? s->idx.n : 18);
		return;
	}
	if (s->extra && !strcmp(path, s->extra)) {
		http_reply(r, 200, "text/html; charset=utf-8", NULL, s->extrahtml.b, s->extrahtml.n);
		return;
	}
	/* a page: /TOPIC.html, or a directory's index */
	Buf topic = {0};
	bufputs(&topic, path + (*path == '/'));
	if (!topic.n || topic.b[topic.n - 1] == '/')
		bufputs(&topic, "index");
	else if (hassuffix(topic.b, ".html"))
		topic.b[topic.n -= 5] = 0;
	if (!strstr(topic.b, "..")) {
		if (s->root && !strncmp(topic.b, "ipl/", 4)) {
			const char *nm = topic.b + 4;
			char *md = !strcmp(nm, "index") ? iplindexmd(s->root) : iplmd(s->root, nm);
			if (md) {
				replyhtml(r, 200, topic.b, md);
				return;
			}
		} else if (s->t) {
			Page *p = findtopic(s->t, topic.b);
			if (p) {
				Buf out = {0};
				htmlpage(&out, p, s->docs[p - s->t->pages.v]);
				http_reply(r, 200, "text/html; charset=utf-8", NULL, out.b, out.n);
				return;
			}
		}
	}
	Buf md = {0};
	bufputs(&md, "# Not found\n\nNo page ");
	bufputs(&md, "`");
	bufputs(&md, path);
	bufputs(&md, "`. Try the search, or the [index](");
	bufputs(&md, relroot(topic.b));
	bufputs(&md, "index.md).\n");
	replyhtml(r, 404, topic.b, md.b);
}

/* newtoken is a secret: 128 random bits, in hex. */
static char *newtoken(void) {
	unsigned char k[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, k, sizeof k) != (ssize_t)sizeof k) {
		fprintf(stderr, "gcon doc: /dev/urandom: %s\n", strerror(errno));
		exit(1);
	}
	close(fd);
	Buf b = {0};
	for (size_t i = 0; i < sizeof k; i++)
		bufprintf(&b, "%02x", k[i]);
	return b.b;
}

/* serveat serves the site on addr (NULL: the loopback address and a
 * free port, and the browser opened at page, a path such as
 * "/ref/strings.html#find"). It returns only if it cannot: 1 if it
 * cannot listen, -1 if there is no browser to open. */
static int serveat(Site *s, const char *addr, const char *page, const char *extramd) {
	bool open = !addr;
	char *bound;
	int fd = http_listen(addr ? addr : "127.0.0.1:0", &bound);
	if (fd < 0)
		return 1;
	if (http_isloopback(bound)) {
		runtoken = newtoken();
		const char *port = strrchr(bound, ':') + 1;
		const char *names[] = {"127.0.0.1", "localhost", "[::1]"};
		VPUSH(s->hosts, bound);
		for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
			Buf h = {0};
			bufprintf(&h, "%s:%s", names[i], port);
			VPUSH(s->hosts, h.b);
		}
	}
	if (s->t)
		searchindex(&s->idx, s->t, s->docs);
	if (s->extra) {
		MdDoc *d = md_parse(extramd, strlen(extramd));
		Page p = {xstrndup(s->extra + 1, strlen(s->extra) - 6), NULL, -1}; /* without / and .html */
		htmlpage(&s->extrahtml, &p, d);
	}
	Buf url = {0};
	bufprintf(&url, "http://%s%s", bound, page);
	if (open && !browse(url.b)) {
		close(fd);
		runtoken = NULL;
		fprintf(stderr, "gcon doc: no browser to open (set BROWSER, or use -http); showing text\n");
		return -1;
	}
	fprintf(stderr, "gcon doc: serving %s (interrupt to stop)\n", url.b);
	if (!runtoken)
		fprintf(stderr, "gcon doc: examples cannot be run when served beyond this machine\n");
	http_serve(fd, serve, s);
}

/* loadsite gives the site the root's tree, if there is a root. */
static void loadsite(Site *s) {
	Root rt = root_find();
	if (!rt.dir)
		return;
	s->root = rt.dir;
	s->t = xcalloc(1, sizeof *s->t);
	*s->t = loadtree(pathjoin(rt.dir, "doc"));
	parseall(s->t, &s->docs);
}

/* browseextra serves a page of the user's own (a program's doc
 * comments, a Markdown file) at path, with the tree; it returns only
 * if it cannot (see serveat). */
static int browseextra(const char *path, const char *md, const char *httpaddr) {
	Site s = {0};
	loadsite(&s);
	s.extra = path;
	return serveat(&s, httpaddr, path, md);
}

/* browsetree serves the tree, opening the browser at page; it returns
 * only if it cannot (see serveat). */
static int browsetree(const char *page, const char *httpaddr) {
	Site s = {0};
	loadsite(&s);
	return serveat(&s, httpaddr, page, NULL);
}

static _Noreturn void docusage(void) { help_usage("doc"); }

/* manname is a page's name for .TH: gcon-TOPIC, a / as -. */
static char *manname(const char *topic) {
	Buf b = {0};
	bufputs(&b, "gcon-");
	for (; *topic; topic++)
		bufputc(&b, *topic == '/' ? '-' : *topic);
	return b.b;
}

int doc_main(int argc, char **argv) {
	enum { Browse, Raw, Html, Man, Text } mode = Browse;
	bool path = false, list = false, check = false, dobuild = false, test = false;
	const char *outdir = NULL, *name = NULL, *decl = NULL, *httpaddr = NULL;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1]) {
			if (name && !decl && hassuffix(name, ".icn"))
				decl = a; /* gcon doc file.icn name */
			else if (name)
				docusage();
			else
				name = a;
			continue;
		}
		if (help_isflag(a))
			help_page("doc");
		const char *f = a + 1 + (a[1] == '-');
		if (!strcmp(f, "raw"))
			mode = Raw;
		else if (!strcmp(f, "html"))
			mode = Html;
		else if (!strcmp(f, "man"))
			mode = Man;
		else if (!strcmp(f, "text"))
			mode = Text;
		else if (!strcmp(f, "path"))
			path = true;
		else if (!strcmp(f, "list"))
			list = true;
		else if (!strcmp(f, "check"))
			check = true;
		else if (!strcmp(f, "test"))
			test = true;
		else if (!strcmp(f, "build"))
			dobuild = true;
		else if (!strcmp(f, "o") && i + 1 < argc)
			outdir = argv[++i];
		else if (!strcmp(f, "http") && i + 1 < argc)
			httpaddr = argv[++i];
		else {
			fprintf(stderr, "flag provided but not defined: -%s\n", f);
			docusage();
		}
	}

	/* an Icon source file: its comment and its declarations' doc comments */
	if (name && hassuffix(name, ".icn") && !check && !dobuild && !list && !test) {
		Buf b = {0};
		if (!readall(name, &b)) {
			fprintf(stderr, "gcon doc: %s: %s\n", name, strerror(errno));
			return 1;
		}
		char *filedoc;
		DocDecls ds = doccomment_file(b.b, b.n, &filedoc);
		const char *base = strrchr(name, '/');
		base = base ? base + 1 : name;
		Buf md = {0};
		int found = 0;
		if (!decl)
			bufprintf(&md, "# %s\n\n%s%s", base, filedoc, *filedoc ? "\n\n" : "");
		for (size_t i = 0; i < ds.n; i++) {
			if (decl && strcmp(ds.v[i].name, decl))
				continue;
			found++;
			bufprintf(&md, "%s %s\n\n```\n%s\n```\n\n%s%s", decl ? "#" : "##", ds.v[i].name, ds.v[i].decl, ds.v[i].doc, *ds.v[i].doc ? "\n\n" : "");
		}
		if (decl && !found) {
			fprintf(stderr, "gcon doc: %s declares no %s\n", name, decl);
			return 2;
		}
		if (mode == Browse) {
			Buf at = {0};
			bufprintf(&at, "/file/%s.html", base);
			int e = browseextra(at.b, md.b ? md.b : "", httpaddr);
			if (e > 0)
				return e;
		}
		MdDoc *d = md_parse(md.b ? md.b : "", md.n);
		Buf out = {0};
		switch (mode) {
		case Raw: fputs(md.b ? md.b : "", stdout); break;
		case Html: {
			Page p = {(char *)base, (char *)name, -1};
			htmlpage(&out, &p, d);
			fputs(out.b, stdout);
			break;
		}
		case Man:
			md_roff(&out, d, manname(decl ? decl : base), "Gcon " GCON_VERSION);
			fputs(out.b, stdout);
			break;
		case Browse: /* a program's own: as text */
		case Text:
			md_text(&out, d, TextWidth);
			pager(out.b);
		}
		return 0;
	}
	if (decl)
		docusage();

	/* a Markdown file named by its path, outside the tree */
	if (name && hassuffix(name, ".md") && access(name, R_OK) == 0 && !check && !dobuild && !list && !path && !test) {
		Buf b = {0};
		if (!readall(name, &b)) {
			fprintf(stderr, "gcon doc: %s: %s\n", name, strerror(errno));
			return 1;
		}
		MdDoc *d = md_parse(b.b, b.n);
		for (size_t k = 0; k < d->errs.n; k++)
			fprintf(stderr, "%s: %s\n", name, d->errs.v[k]);
		Buf out = {0};
		const char *base = strrchr(name, '/');
		base = base ? base + 1 : name;
		char *topic = xstrndup(base, strlen(base) - 3);
		if (mode == Browse) {
			Buf at = {0};
			bufprintf(&at, "/file/%s.html", topic);
			int e = browseextra(at.b, b.b, httpaddr);
			if (e > 0)
				return e;
		}
		switch (mode) {
		case Raw: fputs(b.b, stdout); break;
		case Html: {
			Page p = {topic, (char *)name, -1};
			htmlpage(&out, &p, d);
			fputs(out.b, stdout);
			break;
		}
		case Man:
			md_roff(&out, d, manname(topic), "Gcon " GCON_VERSION);
			fputs(out.b, stdout);
			break;
		case Browse:
		case Text:
			md_text(&out, d, TextWidth);
			pager(out.b);
			break;
		}
		return d->errs.n ? 1 : 0;
	}

	Root root = root_find();
	if (!root.dir) {
		fprintf(stderr, "gcon doc: no GCONROOT found, so no documentation; see gcon env\n");
		return 1;
	}
	char *dir = pathjoin(root.dir, "doc");
	Tree t = loadtree(dir);
	char *html = outdir ? (char *)outdir : pathjoin(dir, "html");

	if (list) {
		MdDoc **docs;
		parseall(&t, &docs);
		for (size_t i = 0; i < t.pages.n; i++)
			printf("%-28s %s\n", t.pages.v[i].topic, docs[i]->title);
		return 0;
	}
	if (check || dobuild) {
		if (name)
			docusage();
		MdDoc **docs;
		int bad = parseall(&t, &docs);
		bad += checkall(&t, docs);
		if (bad) {
			fprintf(stderr, "gcon doc: %d problem%s\n", bad, bad == 1 ? "" : "s");
			return 1;
		}
		return dobuild ? build(&t, docs, html) : 0;
	}

	if (test) {
		Target tg, *only = NULL;
		if (name) {
			tg = lookup(&t, name);
			if (!tg.page) {
				fprintf(stderr, "gcon doc %s: no such page or entry\n", name);
				return 2;
			}
			only = &tg;
		}
		int nrun = 0, ntrans = 0;
		int bad = testexamples(&t, only, &nrun, &ntrans);
		printf("gcon doc: %d example%s run, %d translated; %d failed\n", nrun, nrun == 1 ? "" : "s", ntrans, bad);
		return bad ? 1 : 0;
	}

	/* the Icon Program Library: shown apart from the tree */
	if (name && !strcmp(name, "ipl")) {
		if (mode == Browse && browsetree("/ipl/index.html", httpaddr) > 0)
			return 1;
		ipllist(root.dir);
		return 0;
	}
	Target tg = {NULL, NULL, -1};
	if (!name || strncmp(name, "ipl/", 4))
		tg = lookup(&t, name ? name : "index");
	if (!tg.page) {
		char *md = iplmd(root.dir, name);
		if (!md) {
			fprintf(stderr, "gcon doc %s: no such page, entry or IPL procedure; gcon doc -list lists the pages, gcon doc ipl the IPL\n", name);
			return 2;
		}
		if (path) {
			fprintf(stderr, "gcon doc %s: the IPL has no HTML\n", name);
			return 1;
		}
		if (mode == Browse) {
			char *nm = xstrndup(name, strlen(name));
			if (!strncmp(nm, "ipl/", 4))
				nm += 4;
			if (hassuffix(nm, ".icn"))
				nm[strlen(nm) - 4] = 0;
			Buf at = {0};
			bufprintf(&at, "/ipl/%s.html", nm);
			if (browsetree(at.b, httpaddr) > 0)
				return 1;
		}
		MdDoc *d = md_parse(md, strlen(md));
		Buf out = {0};
		switch (mode) {
		case Raw: fputs(md, stdout); break;
		case Html: {
			Page p = {"ipl", NULL, -1};
			htmlpage(&out, &p, d);
			fputs(out.b, stdout);
			break;
		}
		case Man:
			md_roff(&out, d, manname(d->title), "Gcon " GCON_VERSION);
			fputs(out.b, stdout);
			break;
		case Browse: /* no browser: as text */
		case Text:
			md_text(&out, d, TextWidth);
			pager(out.b);
		}
		return 0;
	}
	Page *p = tg.page;
	size_t n;
	char *src = source(p, &n);
	MdDoc *d = tg.doc ? tg.doc : md_parse(src, n);
	const char *anchor = NULL, *mname = manname(p->topic);
	if (tg.entry >= 0) {
		/* one reference entry: shown alone, or at its anchor */
		anchor = d->blocks.v[tg.entry].anchor;
		src = entrysource(src, d, tg.entry, true);
		mname = manname(d->blocks.v[tg.entry].anchor);
		d = entrydoc(d, tg.entry);
	}
	Buf out = {0};
	Buf file = {0};
	bufprintf(&file, "%s/%s.html", html, p->topic);
	if (path) {
		printf("%s\n", file.b);
		return 0;
	}
	switch (mode) {
	case Raw: fputs(src, stdout); return 0;
	case Html:
		htmlpage(&out, p, d);
		fputs(out.b, stdout);
		return 0;
	case Man:
		md_roff(&out, d, mname, "Gcon " GCON_VERSION);
		fputs(out.b, stdout);
		return 0;
	case Browse: {
		Buf at = {0};
		bufprintf(&at, "/%s.html", p->topic);
		if (anchor)
			bufprintf(&at, "#%s", anchor);
		if (browsetree(at.b, httpaddr) > 0)
			return 1;
	}
		/* fall through */
	case Text: md_text(&out, d, TextWidth); pager(out.b);
	}
	return 0;
}
