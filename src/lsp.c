/*
 * The language server (lsp.h): icontools' internal/lsp (server.go,
 * workspace.go, features.go, jsonrpc.go, position.go) and cmd/iconls,
 * on gcon's front end. Its replies are iconls's, byte for byte, where
 * only Icon is concerned (src/tests/lsp.sh); gcon's own parts are its
 * infrastructure (the root's library searched for linked and included
 * files; no ucode), the diagnostics of gcon's name resolution (icont's
 * wording, with the related declaration) and its literals, and its
 * additions to the built-ins.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "lsp.h"

#include "builtin.h"
#include "driver.h"
#include "format.h"
#include "json.h"
#include "lit.h"
#include "sema.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Positions (position.go): LSP positions count UTF-16 code units
 * unless the client agreed to UTF-8 (byte) offsets. */

typedef struct Mapper {
	const char *src;
	size_t n;
	VEC(size_t) starts; /* byte offset of each line start */
	bool utf8;
} Mapper;

typedef struct Pos {
	long line, character;
} Pos;

static void mapper_init(Mapper *m, const char *src, size_t n, bool utf8) {
	m->src = src, m->n = n, m->utf8 = utf8;
	VPUSH(m->starts, 0);
	for (size_t i = 0; i < n; i++)
		if (src[i] == '\n')
			VPUSH(m->starts, i + 1);
}

/* width is a code point's width in position units (Go's range over a
 * string gives U+FFFD for an invalid byte, of UTF-8 length 3). */
static long width(const Mapper *m, int32_t r) {
	if (m->utf8)
		return r < 0x80 ? 1 : r < 0x800 ? 2 : r < 0x10000 ? 3 : 4;
	return r >= 0x10000 && r <= MaxRune ? 2 : 1;
}

static long units(const Mapper *m, const char *s, size_t n) {
	if (m->utf8)
		return (long)n;
	long u = 0;
	for (size_t i = 0; i < n;) {
		int w;
		int32_t r = utf8decode((const uint8_t *)s + i, n - i, &w);
		u += width(m, r);
		i += (size_t)w;
	}
	return u;
}

static Pos position(const Mapper *m, size_t off) {
	if (off > m->n)
		off = m->n;
	size_t lo = 0, hi = m->starts.n; /* the last start <= off */
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if (m->starts.v[mid] <= off)
			lo = mid;
		else
			hi = mid;
	}
	Pos p = {(long)lo, units(m, m->src + m->starts.v[lo], off - m->starts.v[lo])};
	return p;
}

/* offset returns the byte offset of an LSP position, clamped to the line. */
static size_t offsetof_pos(const Mapper *m, Pos p) {
	if (p.line < 0)
		return 0;
	if ((size_t)p.line >= m->starts.n)
		return m->n;
	size_t start = m->starts.v[p.line], end = m->n;
	if ((size_t)p.line + 1 < m->starts.n)
		end = m->starts.v[p.line + 1] - 1; /* the newline */
	long u = 0;
	for (size_t i = start; i < end;) {
		if (u >= p.character)
			return i;
		int w;
		int32_t r = utf8decode((const uint8_t *)m->src + i, end - i, &w);
		u += width(m, r);
		i += (size_t)w;
	}
	return end;
}

static void jpos(Buf *b, Pos p) { bufprintf(b, "{\"line\":%ld,\"character\":%ld}", p.line, p.character); }

static void jrange(Buf *b, const Mapper *m, size_t a, size_t z) {
	bufputs(b, "{\"start\":");
	jpos(b, position(m, a));
	bufputs(b, ",\"end\":");
	jpos(b, position(m, z));
	bufputc(b, '}');
}

/* ------------------------------------------------------------------ */
/* URIs and paths */

static int unhex(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* uritopath converts a file: URI to a path ("" if it is not one), as
 * url.Parse and filepath.Clean do. */
static char *uritopath(const char *uri) {
	if (strncmp(uri, "file:", 5) != 0)
		return xstrndup("", 0);
	const char *p = uri + 5;
	if (!strncmp(p, "//", 2)) {
		p += 2;
		p = strchr(p, '/'); /* past the host */
		if (!p)
			return xstrndup("", 0);
	}
	Buf b = {0};
	for (; *p && *p != '?' && *p != '#'; p++) {
		if (*p == '%' && unhex((unsigned char)p[1]) >= 0 && unhex((unsigned char)p[2]) >= 0) {
			bufputc(&b, unhex((unsigned char)p[1]) * 16 + unhex((unsigned char)p[2]));
			p += 2;
		} else
			bufputc(&b, *p);
	}
	if (!b.b)
		return xstrndup("", 0);
	return pathclean(b.b);
}

/* pathtouri is (&url.URL{Scheme: "file", Path: path}).String(). */
static char *pathtouri(const char *path) {
	Buf b = {0};
	bufputs(&b, "file://");
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		unsigned c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    strchr("-._~$&+,/:;=@", (int)c))
			bufputc(&b, (int)c);
		else
			bufprintf(&b, "%%%02X", c);
	}
	return b.b;
}

static char *dirname_(const char *path) {
	const char *s = strrchr(path, '/');
	if (!s)
		return xstrndup(".", 1);
	if (s == path)
		return xstrndup("/", 1);
	return xstrndup(path, (size_t)(s - path));
}

static const char *basename_(const char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* ------------------------------------------------------------------ */
/* Documents and the workspace (workspace.go) */

typedef struct Doc {
	char *uri, *path; /* path: "" if the URI is not a file: URI */
	long version;
	char *text;
	size_t n;
	File *file;
	Mapper m;
	SemaFile sf;
} Doc;

static Doc *docof(SemaFile *sf) { return (Doc *)((char *)sf - offsetof(Doc, sf)); }

static Doc *newdoc(const char *uri, long version, const char *text, size_t n, bool utf8) {
	Doc *d = xcalloc(1, sizeof *d);
	d->uri = xstrndup(uri, strlen(uri));
	d->path = uritopath(uri);
	d->version = version;
	d->text = xmalloc(n + 1);
	memcpy(d->text, text, n);
	d->text[n] = 0;
	d->n = n;
	d->file = syn_parse(d->text, n);
	mapper_init(&d->m, d->text, n, utf8);
	d->sf.path = d->path[0] ? d->path : d->uri;
	d->sf.syn = d->file;
	return d;
}

typedef struct DiskFile {
	long long mod;
	long long size;
	Doc *doc;
} DiskFile;

typedef struct Workspace {
	StrMap open;  /* path -> Doc *, open in the editor */
	StrMap cache; /* path -> DiskFile * */
	StrVec ipath; /* directories the client names for linked files */
	bool utf8;
	const char *root; /* gcon's root, or NULL */
} Workspace;

static long long mtime(const struct stat *st) {
#if defined(__APPLE__)
	return (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#else
	return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

/* load returns the document for path: the open one, or the file on
 * disk (cached by modification time and size). */
static Doc *load(Workspace *w, const char *path) {
	Doc *d = strmap_get(&w->open, path, strlen(path));
	if (d)
		return d;
	struct stat st;
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
		return NULL;
	DiskFile *c = strmap_get(&w->cache, path, strlen(path));
	if (c && c->mod == mtime(&st) && c->size == (long long)st.st_size)
		return c->doc;
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	Buf b = {0};
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&b, chunk, k);
	fclose(f);
	if (!b.b)
		bufputs(&b, "");
	d = newdoc(pathtouri(path), 0, b.b, b.n, w->utf8);
	c = xcalloc(1, sizeof *c);
	c->mod = mtime(&st), c->size = (long long)st.st_size, c->doc = d;
	strmap_put(&w->cache, path, strlen(path), c);
	return d;
}

static const char *tokname(Doc *d, Token *t, size_t *n) {
	*n = t->len;
	return d->text + t->offset;
}

/* definesmain reports whether d declares procedure main. */
static bool definesmain(Doc *d) {
	for (size_t i = 0; i < d->file->decls.n; i++) {
		Node *x = d->file->decls.v[i];
		if (x->kind == N_PROC && x->proc.name && x->proc.name->len == 4 &&
		    !memcmp(d->text + x->proc.name->offset, "main", 4))
			return true;
	}
	return false;
}

/* splitpath splits an environment search path at spaces and colons, as
 * gcon (and icont) do. */
static void splitpath(StrVec *v, const char *s) {
	while (s && *s) {
		while (*s == ' ' || *s == ':')
			s++;
		const char *e = s;
		while (*e && *e != ' ' && *e != ':')
			e++;
		if (e > s)
			VPUSH(*v, xstrndup(s, (size_t)(e - s)));
		s = e;
	}
}

/* linkdirs returns the directories searched for a file linked from
 * from: its own, the client's ipath, IPATH, then the root's library. */
static StrVec linkdirs(Workspace *w, Doc *from) {
	StrVec v = {0};
	if (from->path[0])
		VPUSH(v, dirname_(from->path));
	for (size_t i = 0; i < w->ipath.n; i++)
		VPUSH(v, w->ipath.v[i]);
	splitpath(&v, getenv("IPATH"));
	if (w->root) {
		VPUSH(v, pathjoin(w->root, "lib/ipl/procs"));
		VPUSH(v, pathjoin(w->root, "lib/ipl/gprocs"));
	}
	return v;
}

/* findlink locates the source of a linked file, or NULL. */
static Doc *findlink(Workspace *w, const char *name, Doc *from) {
	size_t n = strlen(name);
	char *nm = n >= 4 && !strcmp(name + n - 4, ".icn") ? xstrndup(name, n - 4) : xstrndup(name, n);
	StrVec dirs = linkdirs(w, from);
	for (size_t i = 0; i < dirs.n; i++) {
		char *base = nm[0] == '/' ? xstrndup(nm, strlen(nm)) : pathjoin(dirs.v[i], nm);
		Buf p = {0};
		bufputs(&p, base);
		bufputs(&p, ".icn");
		Doc *d = load(w, p.b);
		if (d)
			return d;
	}
	return NULL;
}

/* findinclude locates a $include file: relative to the including file,
 * then in the client's ipath, LPATH and the root's include files. */
static Doc *findinclude(Workspace *w, const char *name, Doc *from) {
	if (name[0] == '/')
		return load(w, name);
	StrVec dirs = {0};
	if (from->path[0])
		VPUSH(dirs, dirname_(from->path));
	for (size_t i = 0; i < w->ipath.n; i++)
		VPUSH(dirs, w->ipath.v[i]);
	splitpath(&dirs, getenv("LPATH"));
	if (w->root) {
		VPUSH(dirs, pathjoin(w->root, "lib/ipl/incl"));
		VPUSH(dirs, pathjoin(w->root, "lib/ipl/gincl"));
	}
	for (size_t i = 0; i < dirs.n; i++) {
		Doc *d = load(w, pathjoin(dirs.v[i], name));
		if (d)
			return d;
	}
	return NULL;
}

/* ---- preprocessor symbols */

static bool iswordbyte(int c) {
	return c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}

static char *trimspace(const char *s, size_t n) {
	while (n && strchr(" \t\n\v\f\r", *s) && *s)
		s++, n--;
	while (n && strchr(" \t\n\v\f\r", s[n - 1]) && s[n - 1])
		n--;
	return xstrndup(s, n);
}

/* stripcomment removes a trailing # comment, ignoring # inside quotes. */
static char *stripcomment(const char *s, size_t n) {
	char q = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (q && c == '\\')
			i++;
		else if (q && c == q)
			q = 0;
		else if (!q && (c == '"' || c == '\''))
			q = c;
		else if (!q && c == '#')
			return trimspace(s, i);
	}
	return trimspace(s, n);
}

/* parsedirective splits a directive line "$cmd name rest # comment". */
static void parsedirective(const char *line, size_t n, char **cmd, char **name, size_t *nameoff, char **rest) {
	const char *dollar = memchr(line, '$', n);
	size_t i = dollar ? (size_t)(dollar - line) + 1 : 0;
#define SKIP() while (i < n && (line[i] == ' ' || line[i] == '\t')) i++
	SKIP();
	size_t j = i;
	while (i < n && iswordbyte((unsigned char)line[i]))
		i++;
	*cmd = xstrndup(line + j, i - j);
	SKIP();
	*nameoff = i;
	j = i;
	while (i < n && iswordbyte((unsigned char)line[i]))
		i++;
	*name = xstrndup(line + j, i - j);
	*rest = stripcomment(line + i, n - i);
#undef SKIP
}

/* The macro scopes of one file being computed (macros' frames: an
 * $include makes one for the included file). */
typedef struct MacroFrame {
	Doc *d;
	size_t tok, triv;   /* the next trivia to look at */
	VEC(MacroScope) out;
	StrMap active;      /* name -> index in out + 1 */
	size_t incfrom;     /* the parent's $include offset */
} MacroFrame;

static void define(MacroFrame *f, MacroDef *def, size_t from) {
	size_t i = (size_t)(uintptr_t)strmap_get(&f->active, def->name, strlen(def->name));
	if (i)
		f->out.v[i - 1].until = from;
	strmap_put(&f->active, def->name, strlen(def->name), (void *)(uintptr_t)(f->out.n + 1));
	MacroScope s = {def, from, SIZE_MAX};
	VPUSH(f->out, s);
}

/* macros computes the preprocessor symbols in force in each part of d,
 * following $define, $undef and $include directives in order, without
 * recursion (an explicit stack of included files; the stack guards
 * against include cycles). */
static void macros(Workspace *w, Doc *d) {
	VEC(MacroFrame) stk = {0};
	MacroFrame f0 = {0};
	f0.d = d;
	VPUSH(stk, f0);
	StrMap onstack = {0};
	strmap_put(&onstack, d->path, strlen(d->path), (void *)1);
	for (;;) {
		MacroFrame *f = &stk.v[stk.n - 1];
		Doc *x = f->d;
		bool pushed = false;
		while (f->tok < x->file->toks.n && !pushed) {
			Token *t = &x->file->toks.v[f->tok];
			if (f->triv >= t->nlead) {
				f->tok++, f->triv = 0;
				continue;
			}
			Trivia *tr = &t->lead[f->triv++];
			if (tr->kind != TV_DIRECTIVE)
				continue;
			char *cmd, *name, *rest;
			size_t nameoff;
			parsedirective(x->text + tr->offset, tr->len, &cmd, &name, &nameoff, &rest);
			if (!strcmp(cmd, "define")) {
				if (!name[0])
					continue;
				MacroDef *def = xcalloc(1, sizeof *def);
				def->name = name, def->value = rest, def->path = x->path;
				def->offset = tr->offset + nameoff, def->end = def->offset + strlen(name);
				define(f, def, tr->offset);
			} else if (!strcmp(cmd, "undef")) {
				size_t i = (size_t)(uintptr_t)strmap_get(&f->active, name, strlen(name));
				if (i) {
					f->out.v[i - 1].until = tr->offset;
					strmap_put(&f->active, name, strlen(name), (void *)0);
				}
			} else if (!strcmp(cmd, "include")) {
				Buf both = {0};
				bufputs(&both, name);
				bufputs(&both, rest);
				char *file = trimspace(both.b ? both.b : "", both.n);
				size_t fn = strlen(file), a = 0;
				while (a < fn && file[a] == '"')
					a++;
				while (fn > a && file[fn - 1] == '"')
					fn--;
				Doc *inc = findinclude(w, xstrndup(file + a, fn - a), x);
				if (!inc || strmap_get(&onstack, inc->path, strlen(inc->path)))
					continue;
				strmap_put(&onstack, inc->path, strlen(inc->path), (void *)1);
				MacroFrame c = {0};
				c.d = inc;
				c.incfrom = tr->offset;
				VPUSH(stk, c);
				pushed = true;
			}
		}
		if (pushed)
			continue;
		/* this file is done */
		MacroFrame done = stk.v[--stk.n];
		if (!stk.n) {
			d->sf.macros = done.out.v;
			d->sf.nmacros = done.out.n;
			break;
		}
		strmap_put(&onstack, done.d->path, strlen(done.d->path), (void *)0);
		MacroFrame *parent = &stk.v[stk.n - 1];
		for (size_t i = 0; i < done.out.n; i++)
			if (done.out.v[i].until == SIZE_MAX) /* still defined at the end of the included file */
				define(parent, done.out.v[i].def, done.incfrom);
	}
	free(stk.v);
}

/* links returns the names in d's link declarations. */
static StrVec links(Doc *d) {
	StrVec v = {0};
	for (size_t i = 0; i < d->file->decls.n; i++) {
		Node *x = d->file->decls.v[i];
		if (x->kind != N_LINK)
			continue;
		for (size_t k = 0; k < x->link.files.elems.n; k++) {
			Token *t = x->link.files.elems.v[k];
			if (!t)
				continue;
			const char *s = d->text + t->offset;
			size_t n = t->len;
			while (n && *s == '"')
				s++, n--;
			while (n && s[n - 1] == '"')
				n--;
			VPUSH(v, xstrndup(s, n));
		}
	}
	return v;
}

/* program assembles the program containing d: d itself; the other .icn
 * files in its directory, except those defining main when d does (they
 * are other programs); and, transitively, the files they link. */
typedef VEC(Doc *) DocVec;

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct OpenCtx {
	const char *dir;
	StrVec *paths;
} OpenCtx;

static void opendir_each(void *ctx, const char *key, size_t n, void *val) {
	OpenCtx *c = ctx;
	if (!val)
		return;
	char *p = xstrndup(key, n);
	char *d = dirname_(p);
	if (!strcmp(d, c->dir) && n >= 4 && !strcmp(p + n - 4, ".icn"))
		VPUSH(*c->paths, p);
}

static Program *program(Workspace *w, Doc *d, DocVec *order) {
	StrMap seen = {0};
	order->n = 0;
#define ADD(x)                                                                                 \
	do {                                                                                   \
		const char *key = (x)->path[0] ? (x)->path : (x)->uri;                         \
		if (!strmap_get(&seen, key, strlen(key))) {                                    \
			strmap_put(&seen, key, strlen(key), (x));                              \
			VPUSH(*order, (x));                                                    \
		}                                                                              \
	} while (0)
	ADD(d);
	if (d->path[0]) {
		char *dir = dirname_(d->path);
		bool mainhere = definesmain(d);
		StrVec paths = {0};
		DIR *dh = opendir(dir);
		if (dh) {
			struct dirent *e;
			while ((e = readdir(dh))) {
				size_t n = strlen(e->d_name);
				if (n < 4 || strcmp(e->d_name + n - 4, ".icn"))
					continue;
				char *p = pathjoin(dir, e->d_name);
				struct stat st;
				if (lstat(p, &st) == 0 && !S_ISDIR(st.st_mode))
					VPUSH(paths, p);
			}
			closedir(dh);
		}
		OpenCtx oc = {dir, &paths};
		strmap_each(&w->open, opendir_each, &oc);
		qsort(paths.v, paths.n, sizeof *paths.v, cmpstr);
		for (size_t i = 0; i < paths.n; i++) {
			Doc *x = load(w, paths.v[i]);
			if (x && !(mainhere && x != d && definesmain(x)))
				ADD(x);
		}
	}
	bool complete = true;
	for (size_t i = 0; i < order->n; i++) {
		StrVec names = links(order->v[i]);
		for (size_t k = 0; k < names.n; k++) {
			Doc *x = findlink(w, names.v[k], order->v[i]);
			if (x)
				ADD(x);
			else
				complete = false;
		}
	}
#undef ADD
	SemaFile **files = xcalloc(order->n + 1, sizeof *files);
	for (size_t i = 0; i < order->n; i++) {
		macros(w, order->v[i]);
		files[i] = &order->v[i]->sf;
	}
	return sema_program(files, order->n, complete);
}

/* ------------------------------------------------------------------ */
/* The connection (jsonrpc.go) */

enum {
	CodeParseError = -32700,
	CodeInvalidParams = -32602,
	CodeMethodNotFound = -32601,
	CodeInternalError = -32603,
	CodeServerNotInitialized = -32002,
	CodeRequestFailed = -32803,
};

typedef struct Server {
	FILE *in, *out, *logf;
	StrMap docs; /* uri -> Doc *, open */
	Workspace ws;
	bool initialized, shutdown, utf8, hierarchical;
	const char *docsdir; /* reference pages, or NULL */
} Server;

static void logf_(Server *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void logf_(Server *s, const char *fmt, ...) {
	time_t now = time(NULL);
	char ts[32];
	strftime(ts, sizeof ts, "%Y/%m/%d %H:%M:%S", localtime(&now));
	fprintf(s->logf, "gcon lsp: %s ", ts);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(s->logf, fmt, ap);
	va_end(ap);
	fputc('\n', s->logf);
	fflush(s->logf);
}

static void send(Server *s, Buf *body) {
	fprintf(s->out, "Content-Length: %zu\r\n\r\n", body->n);
	fwrite(body->b, 1, body->n, s->out);
	fflush(s->out);
}

/* reply sends a result (JSON text) or an error for request id. */
static void reply(Server *s, JVal *id, const char *result, int code, const char *msg) {
	Buf b = {0};
	bufputs(&b, "{\"jsonrpc\":\"2.0\"");
	if (id) {
		bufputs(&b, ",\"id\":");
		json_compact(&b, id);
	}
	if (msg) {
		bufprintf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
		json_stringz(&b, msg);
		bufputc(&b, '}');
	} else {
		bufputs(&b, ",\"result\":");
		bufputs(&b, result);
	}
	bufputc(&b, '}');
	send(s, &b);
}

static void notify(Server *s, const char *method, const char *params) {
	Buf b = {0};
	bufputs(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
	json_stringz(&b, method);
	bufputs(&b, ",\"params\":");
	bufputs(&b, params);
	bufputc(&b, '}');
	send(s, &b);
}

/* readmsg reads one message: a header block with a Content-Length, then
 * that many bytes of JSON. It returns the body, NULL at end of input,
 * with *bad set if the header was malformed. */
static char *readmsg(Server *s, size_t *n, bool *bad) {
	char line[4096];
	long len = -1;
	bool any = false;
	*bad = false;
	while (fgets(line, sizeof line, s->in)) {
		any = true;
		size_t k = strlen(line);
		while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
			line[--k] = 0;
		if (k == 0)
			break;
		char *colon = strchr(line, ':');
		if (colon && (size_t)(colon - line) == 14 && !strncasecmp(line, "Content-Length", 14)) {
			char *e;
			len = strtol(colon + 1, &e, 10);
			while (*e == ' ' || *e == '\t')
				e++;
			if (*e || len < 0)
				len = -2;
		}
	}
	if (!any)
		return NULL;
	if (len < 0) {
		*bad = true;
		return NULL;
	}
	char *body = xmalloc((size_t)len + 1);
	if (fread(body, 1, (size_t)len, s->in) != (size_t)len) {
		free(body);
		return NULL;
	}
	body[len] = 0;
	*n = (size_t)len;
	return body;
}

/* ------------------------------------------------------------------ */
/* Diagnostics */

/* tokenat returns the non-implicit token starting at byte offset off. */
static Token *tokenat(File *f, size_t off) {
	size_t lo = 0, hi = f->toks.n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (f->toks.v[mid].offset >= off)
			hi = mid;
		else
			lo = mid + 1;
	}
	for (; lo < f->toks.n && f->toks.v[lo].offset == off; lo++)
		if (!f->toks.v[lo].implicit)
			return &f->toks.v[lo];
	return NULL;
}

static void diagnostic(Buf *b, const Mapper *m, size_t a, size_t z, int sev, const char *msg) {
	bufputs(b, "{\"range\":");
	jrange(b, m, a, z);
	bufprintf(b, ",\"severity\":%d,\"source\":\"gcon\",\"message\":", sev);
	json_stringz(b, msg);
}

/* diagnostics returns d's syntax errors or, if it has none, the semantic
 * problems found by resolving its names in its program. */
static void diagnostics(Server *s, Doc *d, Buf *b) {
	bufputc(b, '[');
	bool first = true;
	if (d->file->errs.n == 0) {
		DocVec order = {0};
		Program *p = program(&s->ws, d, &order);
		SemaInfo *info = sema_resolve(p, &d->sf);
		for (size_t i = 0; i < info->diags.n; i++) {
			Diagnostic *e = &info->diags.v[i];
			if (!first)
				bufputc(b, ',');
			first = false;
			diagnostic(b, &d->m, e->offset, e->end, e->severity == SEV_WARNING ? 2 : 1, e->msg);
			if (e->related && e->related->file && e->related->decl) {
				/* gcon: the declaration it conflicts with */
				Doc *rd = docof(e->related->file);
				bufputs(b, ",\"relatedInformation\":[{\"location\":{\"uri\":");
				json_stringz(b, rd->uri);
				bufputs(b, ",\"range\":");
				jrange(b, &rd->m, e->related->decl->offset, e->related->decl->offset + e->related->decl->len);
				bufputs(b, "},\"message\":");
				Buf m = {0};
				bufprintf(&m, "the other declaration: %s %s", sym_kind_name(e->related->kind), e->related->name);
				json_stringz(b, m.b);
				bufputs(b, "}]");
			}
			bufputc(b, '}');
		}
		bufputc(b, ']');
		return;
	}
	for (size_t i = 0; i < d->file->errs.n; i++) {
		SynError *e = &d->file->errs.v[i];
		size_t start = e->offset, end = e->offset;
		Token *t = tokenat(d->file, e->offset);
		if (t)
			end = t->offset + t->len;
		if (!first)
			bufputc(b, ',');
		first = false;
		diagnostic(b, &d->m, start, end, 1, e->msg);
		bufputc(b, '}');
	}
	bufputc(b, ']');
}

static int cmpuri(const void *a, const void *b) {
	return strcmp((*(Doc *const *)a)->uri, (*(Doc *const *)b)->uri);
}

static void collectdoc(void *ctx, const char *key, size_t n, void *val) {
	(void)key, (void)n;
	if (val)
		VPUSH(*(DocVec *)ctx, (Doc *)val);
}

static DocVec opendocs(Server *s) {
	DocVec v = {0};
	strmap_each(&s->docs, collectdoc, &v);
	qsort(v.v, v.n, sizeof *v.v, cmpuri);
	return v;
}

static void publishall(Server *s) {
	DocVec v = opendocs(s);
	for (size_t i = 0; i < v.n; i++) {
		Doc *d = v.v[i];
		Buf p = {0};
		bufputs(&p, "{\"uri\":");
		json_stringz(&p, d->uri);
		bufprintf(&p, ",\"version\":%ld,\"diagnostics\":", d->version);
		diagnostics(s, d, &p);
		bufputc(&p, '}');
		notify(s, "textDocument/publishDiagnostics", p.b);
	}
	free(v.v);
}

/* update records new text for a document, reparses it and publishes
 * diagnostics for all open documents (a change to one file can affect
 * the names declared in others). */
static void update(Server *s, const char *uri, long version, const char *text, size_t n) {
	Doc *d = newdoc(uri, version, text, n, s->utf8);
	strmap_put(&s->docs, uri, strlen(uri), d);
	if (d->path[0])
		strmap_put(&s->ws.open, d->path, strlen(d->path), d);
	publishall(s);
}

/* ------------------------------------------------------------------ */
/* Formatting */

/* formatting returns the edit that formats d, or no edits if d has
 * syntax errors. The edit replaces only the lines that differ. */
static void formatting(Doc *d, Buf *b) {
	Buf out = {0};
	if (d->file->errs.n || !format_source(d->text, d->n, &out) || (out.n == d->n && !memcmp(out.b, d->text, d->n))) {
		bufputs(b, "[]");
		return;
	}
	const char *a = d->text, *z = out.b;
	size_t an = d->n, zn = out.n, p = 0, q = 0;
	while (p < an && p < zn && a[p] == z[p])
		p++;
	while (p > 0 && a[p - 1] != '\n')
		p--;
	while (q < an - p && q < zn - p && a[an - 1 - q] == z[zn - 1 - q])
		q++;
	const char *nl = memchr(a + an - q, '\n', q);
	if (nl)
		q -= (size_t)(nl - (a + an - q)) + 1;
	else
		q = 0;
	bufputs(b, "[{\"range\":");
	jrange(b, &d->m, p, an - q);
	bufputs(b, ",\"newText\":");
	json_string(b, z + p, zn - q - p);
	bufputs(b, "}]");
}

/* ------------------------------------------------------------------ */
/* Document symbols */

enum { SymbolModule = 2, SymbolField = 8, SymbolFunction = 12, SymbolVariable = 13, SymbolStruct = 23 };

typedef struct DSym {
	char *name, *detail;
	int kind;
	size_t ra, rz, sa, sz; /* range, selection range */
	VEC(struct DSym) children;
} DSym;

typedef VEC(DSym) DSymVec;

static char *unquote(const char *s, size_t n) {
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
		return xstrndup(s + 1, n - 2);
	return xstrndup(s, n);
}

static Token *firsttoken(Node *n);
static Token *lasttoken(Node *n);

static bool mksym(Doc *d, Token *t, int kind, const char *detail, Node *whole, DSym *out) {
	if (!t || t->kind == T_ILLEGAL)
		return false;
	memset(out, 0, sizeof *out);
	out->name = unquote(d->text + t->offset, t->len);
	out->detail = xstrndup(detail, strlen(detail));
	out->kind = kind;
	out->sa = t->offset, out->sz = t->offset + t->len;
	out->ra = out->sa, out->rz = out->sz;
	if (whole) {
		Token *a = firsttoken(whole), *z = lasttoken(whole);
		if (a && z)
			out->ra = a->offset, out->rz = z->offset + z->len;
	}
	return true;
}

/* paramlist renders "(a, b, c[])". */
static char *paramlist(Doc *d, TokList *l, bool variadic) {
	Buf b = {0};
	bufputc(&b, '(');
	bool first = true;
	for (size_t i = 0; i < l->elems.n; i++) {
		Token *t = l->elems.v[i];
		if (!t)
			continue;
		if (!first)
			bufputs(&b, ", ");
		first = false;
		bufadd(&b, d->text + t->offset, t->len);
	}
	if (variadic)
		bufputs(&b, "[]");
	bufputc(&b, ')');
	return b.b;
}

static DSymVec docsymbols(Doc *d) {
	DSymVec syms = {0};
	DSym s, c;
	for (size_t i = 0; i < d->file->decls.n; i++) {
		Node *x = d->file->decls.v[i];
		switch (x->kind) {
		case N_LINK:
			for (size_t k = 0; k < x->link.files.elems.n; k++)
				if (mksym(d, x->link.files.elems.v[k], SymbolModule, "link", NULL, &s))
					VPUSH(syms, s);
			break;
		case N_GLOBAL:
			for (size_t k = 0; k < x->global.names.elems.n; k++)
				if (mksym(d, x->global.names.elems.v[k], SymbolVariable, "global", NULL, &s))
					VPUSH(syms, s);
			break;
		case N_RECORD: {
			Buf det = {0};
			bufputs(&det, "record");
			bufputs(&det, paramlist(d, &x->record.fields, false));
			if (!mksym(d, x->record.name, SymbolStruct, det.b, x, &s))
				break;
			for (size_t k = 0; k < x->record.fields.elems.n; k++)
				if (mksym(d, x->record.fields.elems.v[k], SymbolField, "field", NULL, &c))
					VPUSH(s.children, c);
			VPUSH(syms, s);
			break;
		}
		case N_PROC: {
			Buf det = {0};
			bufputs(&det, "procedure");
			bufputs(&det, paramlist(d, &x->proc.params, x->proc.lbrack != NULL));
			if (!mksym(d, x->proc.name, SymbolFunction, det.b, x, &s))
				break;
			for (size_t k = 0; k < x->proc.params.elems.n; k++)
				if (mksym(d, x->proc.params.elems.v[k], SymbolVariable, "parameter", NULL, &c))
					VPUSH(s.children, c);
			for (size_t k = 0; k < x->proc.locals.n; k++) {
				Node *l = x->proc.locals.v[k];
				char *kw = xstrndup(d->text + l->local.keyword->offset, l->local.keyword->len);
				for (size_t j = 0; j < l->local.names.elems.n; j++)
					if (mksym(d, l->local.names.elems.v[j], SymbolVariable, kw, NULL, &c))
						VPUSH(s.children, c);
			}
			VPUSH(syms, s);
			break;
		}
		default:
			break;
		}
	}
	return syms;
}

/* jdsyms writes symbols, hierarchically, without recursion (symbols
 * nest one level). */
static void jdsym(Buf *b, Doc *d, DSym *s, bool children) {
	bufputs(b, "{\"name\":");
	json_stringz(b, s->name);
	if (s->detail[0]) {
		bufputs(b, ",\"detail\":");
		json_stringz(b, s->detail);
	}
	bufprintf(b, ",\"kind\":%d,\"range\":", s->kind);
	jrange(b, &d->m, s->ra, s->rz);
	bufputs(b, ",\"selectionRange\":");
	jrange(b, &d->m, s->sa, s->sz);
	if (children && s->children.n) {
		bufputs(b, ",\"children\":[");
		for (size_t i = 0; i < s->children.n; i++) {
			if (i)
				bufputc(b, ',');
			jdsym(b, d, &s->children.v[i], false);
		}
		bufputc(b, ']');
	}
	bufputc(b, '}');
}

static void jsyminfo(Buf *b, const char *name, int kind, const char *uri, const Mapper *m, size_t a, size_t z,
	const char *container) {
	bufputs(b, "{\"name\":");
	json_stringz(b, name);
	bufprintf(b, ",\"kind\":%d,\"location\":{\"uri\":", kind);
	json_stringz(b, uri);
	bufputs(b, ",\"range\":");
	jrange(b, m, a, z);
	bufputc(b, '}');
	if (container && container[0]) {
		bufputs(b, ",\"containerName\":");
		json_stringz(b, container);
	}
	bufputc(b, '}');
}

static void documentsymbols(Server *s, Doc *d, Buf *b) {
	DSymVec syms = docsymbols(d);
	bufputc(b, '[');
	bool first = true;
	for (size_t i = 0; i < syms.n; i++) {
		if (s->hierarchical) {
			if (i)
				bufputc(b, ',');
			jdsym(b, d, &syms.v[i], true);
			continue;
		}
		/* flattened */
		if (!first)
			bufputc(b, ',');
		first = false;
		jsyminfo(b, syms.v[i].name, syms.v[i].kind, d->uri, &d->m, syms.v[i].sa, syms.v[i].sz, "");
		for (size_t k = 0; k < syms.v[i].children.n; k++) {
			DSym *c = &syms.v[i].children.v[k];
			bufputc(b, ',');
			jsyminfo(b, c->name, c->kind, d->uri, &d->m, c->sa, c->sz, syms.v[i].name);
		}
	}
	bufputc(b, ']');
}

/* The first and last tokens of a node, down its first or last
 * children with tokens. */
typedef struct KidV {
	Token *t;
	Node *n;
} KidV;
typedef VEC(KidV) KidVec;

static void kids(void *ctx, Token *t, Node *n) {
	KidV k = {t, n};
	VPUSH(*(KidVec *)ctx, k);
}

static Token *edge(Node *n, bool first) {
	/* a stack of children still to visit, the next on top */
	KidVec stk = {0};
	KidV r0 = {NULL, n};
	VPUSH(stk, r0);
	while (stk.n) {
		KidV k = stk.v[--stk.n];
		if (k.t) {
			free(stk.v);
			return k.t;
		}
		KidVec ks = {0};
		syn_children(k.n, kids, &ks);
		for (size_t i = 0; i < ks.n; i++)
			VPUSH(stk, ks.v[first ? ks.n - 1 - i : i]);
		free(ks.v);
	}
	free(stk.v);
	return NULL;
}

static Token *firsttoken(Node *n) { return edge(n, true); }
static Token *lasttoken(Node *n) { return edge(n, false); }

/* ------------------------------------------------------------------ */
/* The server (server.go) */

static Doc *getdoc(Server *s, const char *uri) { return strmap_get(&s->docs, uri, strlen(uri)); }

static void initialize(Server *s, JVal *p, Buf *b) {
	s->initialized = true;
	s->hierarchical = json_bool(p, "capabilities.textDocument.documentSymbol.hierarchicalDocumentSymbolSupport");
	JVal *encs = json_get(p, "capabilities.general.positionEncodings");
	for (size_t i = 0; encs && encs->kind == J_ARR && i < encs->items.n; i++)
		if (encs->items.v[i]->kind == J_STR && !strcmp(encs->items.v[i]->s, "utf-8"))
			s->utf8 = true;
	s->ws.utf8 = s->utf8;
	JVal *ip = json_get(p, "initializationOptions.ipath");
	s->ws.ipath.n = 0;
	for (size_t i = 0; ip && ip->kind == J_ARR && i < ip->items.n; i++)
		if (ip->items.v[i]->kind == J_STR)
			VPUSH(s->ws.ipath, ip->items.v[i]->s);
	bufprintf(b,
		"{\"capabilities\":{\"positionEncoding\":\"%s\",\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
		"\"documentSymbolProvider\":true,\"documentFormattingProvider\":true,\"hoverProvider\":true,"
		"\"definitionProvider\":true,\"referencesProvider\":true,\"documentHighlightProvider\":true,"
		"\"workspaceSymbolProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\",\"\\u0026\"]},"
		"\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"],\"retriggerCharacters\":[\",\"]},"
		"\"renameProvider\":{\"prepareProvider\":true}},\"serverInfo\":{\"name\":\"gcon\",\"version\":",
		s->utf8 ? "utf-8" : "utf-16");
	json_stringz(b, GCON_VERSION);
	bufputs(b, "}}");
}

/* The features of stage 2, by request: false if method is not one. */
static bool lsp_feature(Server *s, const char *method, JVal *params, Buf *result, int *code, char **err) {
	(void)s, (void)method, (void)params, (void)result, (void)code, (void)err;
	return false;
}

/* dispatch handles a request or notification; for a request, it writes
 * the result to b (JSON), or sets *err. */
static void dispatch(Server *s, const char *method, JVal *params, Buf *b, int *code, char **err) {
	if (!strcmp(method, "initialize")) {
		initialize(s, params, b);
		return;
	}
	if (!strcmp(method, "initialized")) {
		bufputs(b, "null");
		return;
	}
	if (!strcmp(method, "shutdown")) {
		s->shutdown = true;
		bufputs(b, "null");
		return;
	}
	if (!strcmp(method, "textDocument/didOpen")) {
		JVal *t = json_get(params, "textDocument.text");
		update(s, json_str(params, "textDocument.uri"), json_int(params, "textDocument.version"),
			t && t->kind == J_STR ? t->s : "", t && t->kind == J_STR ? t->n : 0);
		bufputs(b, "null");
		return;
	}
	if (!strcmp(method, "textDocument/didChange")) {
		JVal *ch = json_get(params, "contentChanges");
		if (ch && ch->kind == J_ARR && ch->items.n) {
			JVal *t = json_get(ch->items.v[ch->items.n - 1], "text");
			update(s, json_str(params, "textDocument.uri"), json_int(params, "textDocument.version"),
				t && t->kind == J_STR ? t->s : "", t && t->kind == J_STR ? t->n : 0);
		}
		bufputs(b, "null");
		return;
	}
	if (!strcmp(method, "textDocument/didClose")) {
		const char *uri = json_str(params, "textDocument.uri");
		Doc *d = getdoc(s, uri);
		if (d)
			strmap_put(&s->ws.open, d->path, strlen(d->path), NULL);
		strmap_put(&s->docs, uri, strlen(uri), NULL);
		/* Clear the diagnostics of the closed document, and refresh the
		 * others (the file on disk may differ from the closed buffer). */
		Buf p = {0};
		bufputs(&p, "{\"uri\":");
		json_stringz(&p, uri);
		bufputs(&p, ",\"diagnostics\":[]}");
		notify(s, "textDocument/publishDiagnostics", p.b);
		publishall(s);
		bufputs(b, "null");
		return;
	}
	if (!strcmp(method, "textDocument/documentSymbol") || !strcmp(method, "textDocument/formatting")) {
		const char *uri = json_str(params, "textDocument.uri");
		Doc *d = getdoc(s, uri);
		if (!d) {
			Buf m = {0};
			bufprintf(&m, "document not open: %s", uri);
			*code = CodeInvalidParams, *err = m.b;
			return;
		}
		if (method[13] == 'd')
			documentsymbols(s, d, b);
		else
			formatting(d, b);
		return;
	}
	if (lsp_feature(s, method, params, b, code, err))
		return;
	*code = CodeMethodNotFound;
	Buf m = {0};
	bufprintf(&m, "method not supported: %s", method);
	*err = m.b;
}

static int serve(Server *s) {
	for (;;) {
		size_t n;
		bool bad;
		char *body = readmsg(s, &n, &bad);
		if (!body) {
			if (bad) {
				logf_(s, "bad Content-Length");
				return 1;
			}
			return 0; /* end of input */
		}
		const char *perr;
		JVal *m = json_parse(body, n, &perr);
		if (!m || m->kind != J_OBJ) {
			reply(s, NULL, NULL, CodeParseError, m ? "message is not an object" : perr);
			continue;
		}
		JVal *id = json_get(m, "id");
		if (id && id->kind == J_NULL)
			id = NULL;
		const char *method = json_str(m, "method");
		if (!strcmp(method, "exit"))
			return s->shutdown ? 0 : 1;
		JVal *params = json_get(m, "params");
		if (!s->initialized && strcmp(method, "initialize")) {
			if (id)
				reply(s, id, NULL, CodeServerNotInitialized, "server not initialized");
			continue;
		}
		Buf b = {0};
		int code = 0;
		char *err = NULL;
		dispatch(s, method, params, &b, &code, &err);
		if (err && code == CodeMethodNotFound) {
			/* unknown: an error for a request, unless it is "$/";
			 * ignored as a notification */
			if (!id || !strncmp(method, "$/", 2)) {
				err = NULL;
				b.n = 0;
			}
		}
		if (id) {
			if (err)
				reply(s, id, NULL, code, err);
			else {
				if (!b.b || !b.n) {
					b.n = 0;
					bufputs(&b, "null");
				}
				reply(s, id, b.b, 0, NULL);
			}
		} else if (err)
			logf_(s, "%s: %s", method, err);
		free(b.b);
	}
}

/* ------------------------------------------------------------------ */

static const char *cachedir(void) {
	const char *h = getenv("HOME");
#if defined(__APPLE__)
	if (!h || !*h)
		return NULL;
	Buf b = {0};
	bufprintf(&b, "%s/Library/Caches/gcon/ref", h);
	return b.b;
#else
	const char *x = getenv("XDG_CACHE_HOME");
	Buf b = {0};
	if (x && *x)
		bufprintf(&b, "%s/gcon/ref", x);
	else if (h && *h)
		bufprintf(&b, "%s/.cache/gcon/ref", h);
	else
		return NULL;
	return b.b;
#endif
}

static int lsp_fetchdocs(const char *dir) {
	fprintf(stderr, "gcon lsp: fetch-docs into %s: not yet\n", dir);
	return 1;
}

static _Noreturn void lspusage(void) {
	fputs("usage: gcon lsp [-log file] [-docs dir]\n"
	      "       gcon lsp fetch-docs [-dir dir]\n",
		stderr);
	exit(2);
}

int lsp_main(int argc, char **argv) {
	const char *logfile = NULL, *docs = NULL;
	if (argc > 1 && !strcmp(argv[1], "fetch-docs")) {
		const char *dir = cachedir();
		for (int i = 2; i < argc; i++) {
			if ((!strcmp(argv[i], "-dir") || !strcmp(argv[i], "--dir")) && i + 1 < argc)
				dir = argv[++i];
			else if (!strncmp(argv[i], "-dir=", 5))
				dir = argv[i] + 5;
			else
				lspusage();
		}
		if (!dir) {
			fprintf(stderr, "gcon lsp: no cache directory; use -dir\n");
			return 2;
		}
		return lsp_fetchdocs(dir);
	}
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '-' && a[1] == '-')
			a++;
		if (!strcmp(a, "-log") && i + 1 < argc)
			logfile = argv[++i];
		else if (!strncmp(a, "-log=", 5))
			logfile = a + 5;
		else if (!strcmp(a, "-docs") && i + 1 < argc)
			docs = argv[++i];
		else if (!strncmp(a, "-docs=", 6))
			docs = a + 6;
		else if (!strcmp(a, "-stdio"))
			; /* standard input and output are the only transport */
		else if (!strcmp(a, "-version")) {
			printf("gcon lsp %s\n", GCON_VERSION);
			return 0;
		} else
			lspusage();
	}
	Server s = {0};
	s.in = stdin, s.out = stdout, s.logf = stderr;
	if (logfile && !(s.logf = fopen(logfile, "a"))) {
		fprintf(stderr, "gcon lsp: open %s: %s\n", logfile, strerror(errno));
		return 2;
	}
	s.docsdir = docs ? docs : cachedir();
	s.ws.root = root_find().dir;
	return serve(&s);
}
