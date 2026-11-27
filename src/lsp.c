/*
 * The language server (lsp.h): icontools' internal/lsp (server.go,
 * workspace.go, features.go, jsonrpc.go, position.go) and cmd/iconls,
 * on gcon's front end. Its replies were iconls's, byte for byte, where
 * only Icon was concerned, when it was ported; gcon's own parts are its
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
#include "help.h"
#include "json.h"
#include "lit.h"
#include "refdocs.h"
#include "vet.h"
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
 * from: its own, the client's ipath, IPATH, then the root's library
 * (the IPL's, then gcon's own). */
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
		VPUSH(v, pathjoin(w->root, "lib/gcon")); /* gcon's own library, last */
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
	bool icon95; /* initializationOptions.icon95: vet for Icon 9.5 too */
	const char *docsdir; /* reference pages, or NULL */
	RefDocs *refdocs;
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

/* vetresolve resolves another file of the program, for vet. */
static SemaInfo *vetresolve(void *ctx, SemaFile *f) { return sema_resolve(ctx, f); }

/* vet's link resolver here: where the workspace finds a link
 * declaration's file. VetLinkSelf if it is a file of the program that
 * defines main (the program's own main file); VetLinkGcon if it is in
 * the root's lib/gcon. */
typedef struct VetLinkCtx {
	Workspace *w;
	DocVec *order;
} VetLinkCtx;

static VetLinkKind wslink(void *ctx, SemaInfo *from, Token *name) {
	VetLinkCtx *c = ctx;
	Doc *d = docof(from->file);
	const char *s = d->text + name->offset;
	size_t n = name->len;
	while (n && *s == '"')
		s++, n--;
	while (n && s[n - 1] == '"')
		n--;
	Doc *x = findlink(c->w, xstrndup(s, n), d);
	if (!x)
		return VetLinkOther;
	for (size_t i = 0; i < c->order->n; i++)
		if (c->order->v[i] == x && definesmain(x))
			return VetLinkSelf;
	if (c->w->root && x->path[0]) {
		char *lib = pathjoin(c->w->root, "lib/gcon/");
		if (!strncmp(x->path, lib, strlen(lib)))
			return VetLinkGcon;
	}
	return VetLinkOther;
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
		/* gcon: string and cset literals gcon build would reject (\u and
		 * \U escapes, cset ranges), with its messages */
		for (size_t i = 0; i < d->file->toks.n; i++) {
			Token *t = &d->file->toks.v[i];
			if (t->implicit || (t->kind != T_STRINGLIT && t->kind != T_CSETLIT && t->kind != T_USETLIT))
				continue;
			LitError *err;
			if (t->kind == T_STRINGLIT) {
				char *s;
				size_t n;
				err = lit_string(d->text + t->offset, t->len, &s, &n);
			} else if (t->kind == T_USETLIT) {
				LitUset lu;
				err = lit_uset(d->text + t->offset, t->len, &lu);
				if (!err)
					free(lu.ranges);
			} else {
				LitCset lc;
				err = lit_cset(d->text + t->offset, t->len, &lc);
			}
			if (!err)
				continue;
			if (!first)
				bufputc(b, ',');
			first = false;
			bufputs(b, "{\"range\":");
			jrange(b, &d->m, t->offset, t->offset + t->len);
			bufputs(b, ",\"severity\":1,\"source\":\"gcon\",\"message\":");
			json_string(b, err->msg, err->len);
			bufputc(b, '}');
		}
		/* gcon vet's findings, as warnings */
		VetFindings vf = {0};
		VetLinkCtx lc = {&s->ws, &order};
		VetOptions vo = {s->icon95, wslink, &lc};
		vet_file(p, info, vetresolve, p, vo, &vf);
		for (size_t i = 0; i < vf.n; i++) {
			if (!first)
				bufputc(b, ',');
			first = false;
			bufputs(b, "{\"range\":");
			jrange(b, &d->m, vf.v[i].offset, vf.v[i].end);
			bufputs(b, ",\"severity\":2,\"source\":\"gcon vet\",\"message\":");
			json_stringz(b, vf.v[i].msg);
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
	s->icon95 = json_bool(p, "initializationOptions.icon95");
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

static bool lsp_feature(Server *s, const char *method, JVal *params, Buf *result, int *code, char **err);

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

typedef VEC(char *) CharVec;

/* ------------------------------------------------------------------ */
/* Features (features.go) */

/* goquote writes s as Go's %q (strconv.Quote) does. */
static void goquote(Buf *b, const char *s, size_t n) {
	bufputc(b, '"');
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c < RuneSelf) {
			switch (c) {
			case '"': bufputs(b, "\\\""); break;
			case '\\': bufputs(b, "\\\\"); break;
			case '\a': bufputs(b, "\\a"); break;
			case '\b': bufputs(b, "\\b"); break;
			case '\f': bufputs(b, "\\f"); break;
			case '\n': bufputs(b, "\\n"); break;
			case '\r': bufputs(b, "\\r"); break;
			case '\t': bufputs(b, "\\t"); break;
			case '\v': bufputs(b, "\\v"); break;
			default:
				if (c < 0x20 || c == 0x7f)
					bufprintf(b, "\\x%02x", c);
				else
					bufputc(b, c);
			}
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode((const uint8_t *)s + i, n - i, &w);
		if (r == RuneError && w == 1)
			bufprintf(b, "\\x%02x", c);
		else
			bufadd(b, s + i, (size_t)w);
		i += (size_t)w;
	}
	bufputc(b, '"');
}

static void goquotez(Buf *b, const char *s) { goquote(b, s, strlen(s)); }

/* pathrel is filepath.Rel for two clean absolute paths. */
static char *pathrel(const char *basep, const char *targ) {
	CharVec a = {0}, z = {0};
	char *x = xstrndup(basep, strlen(basep)), *y = xstrndup(targ, strlen(targ));
	for (char *t = strtok(x, "/"); t; t = strtok(NULL, "/"))
		VPUSH(a, t);
	for (char *t = strtok(y, "/"); t; t = strtok(NULL, "/"))
		VPUSH(z, t);
	size_t k = 0;
	while (k < a.n && k < z.n && !strcmp(a.v[k], z.v[k]))
		k++;
	Buf b = {0};
	for (size_t i = k; i < a.n; i++) {
		if (b.n)
			bufputc(&b, '/');
		bufputs(&b, "..");
	}
	for (size_t i = k; i < z.n; i++) {
		if (b.n)
			bufputc(&b, '/');
		bufputs(&b, z.v[i]);
	}
	return b.b ? b.b : xstrndup(".", 1);
}

static char *relpathto(Doc *from, const char *path) {
	if (from->path[0] && path[0])
		return pathrel(dirname_(from->path), path);
	return xstrndup(path, strlen(path));
}

static char *relpath(Doc *from, Doc *to) {
	if (from->path[0] && to->path[0])
		return pathrel(dirname_(from->path), to->path);
	return to->uri;
}

/* posctx is a position in a document, with the document's program and
 * name resolution. */
typedef struct PosCtx {
	Doc *d;
	size_t off;
	Program *prog;
	DocVec docs;
	SemaInfo *info;
	Token *tok; /* identifier at the position, or NULL */
} PosCtx;

static size_t tokidx(Doc *d, Token *t) { return (size_t)(t - d->file->toks.v); }
static const char *ttext(Doc *d, Token *t) { return d->text + t->offset; }
static size_t tend(Token *t) { return t->offset + t->len; }

static bool ttexteq(Doc *d, Token *t, const char *s, size_t n) {
	return t->len == n && !memcmp(ttext(d, t), s, n);
}

/* identat returns the identifier (or keyword name) token touching byte
 * offset off: one containing it, or ending at it. */
static Token *identat(File *f, size_t off) {
	size_t lo = 0, hi = f->toks.n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (f->toks.v[mid].offset > off)
			hi = mid;
		else
			lo = mid + 1;
	}
	for (long j = (long)lo - 1; j >= 0 && j >= (long)lo - 2; j--) {
		Token *t = &f->toks.v[j];
		if (t->implicit)
			continue;
		if ((t->kind == T_IDENT || t->kind == T_FAIL) && t->offset <= off && off <= tend(t))
			return t;
		if (tend(t) < off)
			break;
	}
	return NULL;
}

static bool at(Server *s, JVal *p, PosCtx *c, int *code, char **err) {
	const char *uri = json_str(p, "textDocument.uri");
	Doc *d = getdoc(s, uri);
	if (!d) {
		Buf m = {0};
		bufprintf(&m, "document not open: %s", uri);
		*code = CodeInvalidParams, *err = m.b;
		return false;
	}
	memset(c, 0, sizeof *c);
	c->d = d;
	Pos ps = {json_int(p, "position.line"), json_int(p, "position.character")};
	c->off = offsetof_pos(&d->m, ps);
	c->prog = program(&s->ws, d, &c->docs);
	c->info = sema_resolve(c->prog, &d->sf);
	c->tok = identat(d->file, c->off);
	return true;
}

/* target is what an identifier denotes. */
typedef struct Target {
	Symbol *sym;
	SymVec *fields; /* for a field reference x.name */
	bool isfield;
	const Keyword *kw;
	bool iskw;
	char *link; /* for a link declaration: the linked file's path */
	bool islink;
} Target;

static Target target(Server *s, PosCtx *c) {
	Target t = {0};
	if (!c->tok)
		return t;
	size_t k = tokidx(c->d, c->tok);
	Symbol *sym = c->info->uses[k];
	if (sym) {
		t.sym = sym;
		if (sym->kind == S_FIELD) {
			t.isfield = true;
			t.fields = strmap_get(&c->prog->fields, sym->name, strlen(sym->name));
		}
		return t;
	}
	if (c->info->fielduses[k]) {
		t.isfield = true, t.fields = c->info->fielduses[k];
		return t;
	}
	if (c->info->iskeyword[k]) {
		t.iskw = true, t.kw = c->info->keywords[k];
		return t;
	}
	for (size_t i = 0; i < c->d->file->decls.n; i++) {
		Node *x = c->d->file->decls.v[i];
		if (x->kind != N_LINK)
			continue;
		for (size_t j = 0; j < x->link.files.elems.n; j++)
			if (x->link.files.elems.v[j] == c->tok) {
				t.islink = true;
				const char *n = ttext(c->d, c->tok);
				size_t len = c->tok->len;
				while (len && *n == '"')
					n++, len--;
				while (len && n[len - 1] == '"')
					len--;
				Doc *ld = findlink(&s->ws, xstrndup(n, len), c->d);
				t.link = ld ? ld->path : "";
			}
	}
	return t;
}

static void tokrange(Buf *b, PosCtx *c) { jrange(b, &c->d->m, c->tok->offset, tend(c->tok)); }

/* location writes the location of token t of file f. */
static bool location(Buf *b, SemaFile *f, Token *t) {
	if (!f || !t)
		return false;
	Doc *d = docof(f);
	bufputs(b, "{\"uri\":");
	json_stringz(b, d->uri);
	bufputs(b, ",\"range\":");
	jrange(b, &d->m, t->offset, tend(t));
	bufputc(b, '}');
	return true;
}

/* ---- hover */

static void codeblock(Buf *b, const char *code) {
	bufputs(b, "```icon\n");
	bufputs(b, code);
	bufputs(b, "\n```");
}

static char *toklist(Doc *d, TokList *l) {
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
		bufadd(&b, ttext(d, t), t->len);
	}
	bufputc(&b, ')');
	return b.b;
}

static char *procsignature(Doc *d, Node *p) {
	Buf b = {0};
	if (p->proc.name)
		bufadd(&b, ttext(d, p->proc.name), p->proc.name->len);
	else
		bufputc(&b, '?');
	bufputs(&b, toklist(d, &p->proc.params));
	if (p->proc.lbrack) {
		b.n--; /* the ")" */
		bufputs(&b, "[])");
	}
	return b.b;
}

static char *capitalize(const char *s) {
	char *c = xstrndup(s, strlen(s));
	if (c[0] >= 'a' && c[0] <= 'z')
		c[0] = (char)(c[0] - 'a' + 'A');
	return c;
}

/* doccomment returns the comment documenting a declaration: the
 * comment lines immediately above it, and a trailing comment on its
 * first line (the IPL's "#:" convention). */
static char *doccomment(Doc *d, Symbol *sym) {
	Token *first = firsttoken(sym->node);
	if (!first)
		return xstrndup("", 0);
	CharVec lines = {0};
	int newlines = 0;
	for (size_t i = first->nlead; i-- > 0;) {
		Trivia *tr = &first->lead[i];
		if (tr->kind == TV_SPACE)
			continue;
		if (tr->kind == TV_NEWLINE) {
			if (++newlines > 1)
				break;
			continue;
		}
		if (tr->kind != TV_COMMENT)
			break;
		newlines = 0;
		VPUSH(lines, NULL);
		memmove(lines.v + 1, lines.v, (lines.n - 1) * sizeof *lines.v);
		lines.v[0] = xstrndup(d->text + tr->offset, tr->len);
	}
	long fl = position(&d->m, first->offset).line;
	for (size_t i = tokidx(d, first); i < d->file->toks.n; i++) {
		Token *t = &d->file->toks.v[i];
		if (t->implicit || position(&d->m, t->offset).line == fl)
			continue;
		for (size_t k = 0; k < t->nlead; k++) {
			Trivia *tr = &t->lead[k];
			if (tr->kind == TV_NEWLINE)
				break;
			if (tr->kind == TV_COMMENT && position(&d->m, tr->offset).line == fl)
				VPUSH(lines, xstrndup(d->text + tr->offset, tr->len));
		}
		break;
	}
	Buf b = {0};
	bool any = false;
	for (size_t i = 0; i < lines.n; i++) {
		char *l = lines.v[i];
		while (*l == '#')
			l++;
		if (*l == ':')
			l++;
		bool rule = true;
		for (char *p = l; *p; p++)
			if (!strchr("#=-*_ \t", *p))
				rule = false;
		if (rule)
			continue; /* a rule, not text */
		char *t = trimspace(l, strlen(l));
		if (any)
			bufputs(&b, "  \n");
		any = true;
		bufputs(&b, t);
	}
	return b.b ? b.b : xstrndup("", 0);
}

static void addpart(CharVec *parts, char *s) { VPUSH(*parts, s); }

/* decldoc gives a declaration's doc comment and, if it is in another
 * file, where. */
static void decldoc(Symbol *sym, PosCtx *c, CharVec *parts) {
	if (!sym->file)
		return;
	Doc *d = docof(sym->file);
	char *doc = doccomment(d, sym);
	if (doc[0])
		addpart(parts, doc);
	if (d != c->d) {
		Buf m = {0};
		bufprintf(&m, "Declared in `%s`.", relpath(c->d, d));
		addpart(parts, m.b);
	}
}

static char *joinparts(CharVec *parts) {
	Buf b = {0};
	for (size_t i = 0; i < parts->n; i++) {
		if (i)
			bufputs(&b, "\n\n");
		bufputs(&b, parts->v[i]);
	}
	return b.b ? b.b : xstrndup("", 0);
}

static const char site_[] = "https://www2.cs.arizona.edu/icon/";

static char *functionurl(const BuiltinFunc *f) {
	Buf b = {0};
	if (f->graphics) {
		bufprintf(&b, "%sdocs/ipd281/gipd_a.htm#%s", site_, f->name);
		return b.b;
	}
	char c = f->name[0];
	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	const char *page = c <= 'd' ? "funcad.htm" : c <= 'j' ? "funcej.htm" : c <= 'o' ? "funcko.htm"
			   : c <= 'r' ? "funcpr.htm" : "funcsz.htm";
	bufprintf(&b, "%srefernce/%s#%s", site_, page, f->name);
	return b.b;
}

#include "gcondoc.inc"

/* gcondoc returns the documentation of one of gcon's additions (a
 * keyword with its "&"), or NULL. */
static const char *gcondoc(const char *name) {
	for (size_t i = 0; i < sizeof gcondocs / sizeof gcondocs[0]; i++)
		if (!strcmp(gcondocs[i].name, name))
			return gcondocs[i].md;
	return NULL;
}

static char *builtindoc(Server *s, const BuiltinFunc *f) {
	CharVec parts = {0};
	const char *md = f->gcon ? gcondoc(f->name)
		: s->refdocs ? strmap_get(&s->refdocs->functions, f->name, strlen(f->name)) : NULL;
	if (md)
		addpart(&parts, xstrndup(md, strlen(md)));
	else {
		Buf c = {0};
		codeblock(&c, f->signature);
		addpart(&parts, c.b);
		addpart(&parts, capitalize(f->summary));
	}
	Buf k = {0};
	if (f->gcon)
		bufputs(&k, "Gcon built-in function (not in Icon 9.5) \xc2\xb7 [Gcon design notes](" GCONDOC_URL ")");
	else
		bufprintf(&k, "%s \xc2\xb7 [Icon reference](%s)", f->graphics ? "Built-in graphics function" : "Built-in function",
			functionurl(f));
	addpart(&parts, k.b);
	return joinparts(&parts);
}

static char *keyworddoc(Server *s, const Keyword *k) {
	CharVec parts = {0};
	const char *md = NULL;
	if (k->gcon) {
		Buf n = {0};
		bufprintf(&n, "&%s", k->name);
		md = gcondoc(n.b);
	} else if (s->refdocs)
		md = strmap_get(&s->refdocs->keywords, k->name, strlen(k->name));
	if (md)
		addpart(&parts, xstrndup(md, strlen(md)));
	else {
		Buf c = {0};
		Buf code = {0};
		bufprintf(&code, "&%s", k->name);
		codeblock(&c, code.b);
		addpart(&parts, c.b);
		addpart(&parts, capitalize(k->summary));
	}
	Buf l = {0};
	if (k->gcon)
		bufputs(&l, "Gcon keyword (not in Icon 9.5) \xc2\xb7 [Gcon design notes](" GCONDOC_URL ")");
	else
		bufprintf(&l, "Keyword \xc2\xb7 [Icon reference](%srefernce/keywlist.htm)", site_);
	addpart(&parts, l.b);
	return joinparts(&parts);
}

static char *symboldoc(Server *s, Symbol *sym, PosCtx *c) {
	CharVec parts = {0};
	Buf b = {0};
	switch (sym->kind) {
	case S_PARAM: case S_LOCAL: case S_STATIC: case S_IMPLICIT: {
		const char *word = sym->kind == S_PARAM ? "parameter" : sym->kind == S_STATIC ? "static" : "local";
		Buf code = {0};
		bufprintf(&code, "%s %s", word, sym->name);
		codeblock(&b, code.b);
		addpart(&parts, b.b);
		Buf where = {0};
		Node *p = sym->node;
		if (p && p->kind == N_PROC && p->proc.name) {
			Doc *pd = sym->file ? docof(sym->file) : c->d;
			bufprintf(&where, " of procedure `%.*s`", (int)p->proc.name->len, ttext(pd, p->proc.name));
		}
		Buf t = {0};
		if (sym->kind == S_IMPLICIT)
			bufprintf(&t, "Undeclared, so a local variable%s.", where.b ? where.b : "");
		else
			bufprintf(&t, "%c%s%s.", word[0] - 'a' + 'A', word + 1, where.b ? where.b : "");
		addpart(&parts, t.b);
		break;
	}
	case S_PROCEDURE: {
		Buf code = {0};
		bufprintf(&code, "procedure %s", procsignature(docof(sym->file), sym->node));
		codeblock(&b, code.b);
		addpart(&parts, b.b);
		decldoc(sym, c, &parts);
		break;
	}
	case S_RECORD: {
		Buf code = {0};
		bufprintf(&code, "record %s%s", sym->name, toklist(docof(sym->file), &sym->node->record.fields));
		codeblock(&b, code.b);
		addpart(&parts, b.b);
		decldoc(sym, c, &parts);
		break;
	}
	case S_GLOBAL: {
		Buf code = {0};
		bufprintf(&code, "global %s", sym->name);
		codeblock(&b, code.b);
		addpart(&parts, b.b);
		decldoc(sym, c, &parts);
		break;
	}
	case S_BUILTIN:
		return builtindoc(s, sym->builtin);
	case S_MACRO: {
		MacroDef *m = sym->macro;
		if (m->predefined) {
			Buf code = {0};
			bufprintf(&code, "$define %s 1", m->name);
			codeblock(&b, code.b);
			addpart(&parts, b.b);
			Buf t = {0};
			bufputs(&t, "Predefined by the Icon preprocessor");
			if (m->predefined->feature[0]) {
				bufputs(&t, " when `&features` includes ");
				goquotez(&t, m->predefined->feature);
			}
			bufputc(&t, '.');
			addpart(&parts, t.b);
		} else {
			Buf code = {0};
			bufprintf(&code, "$define %s %s", m->name, m->value);
			char *tc = trimspace(code.b, code.n);
			codeblock(&b, tc);
			addpart(&parts, b.b);
			if (strcmp(m->path, c->d->path)) {
				Buf t = {0};
				bufprintf(&t, "Defined in `%s`.", relpathto(c->d, m->path));
				addpart(&parts, t.b);
			}
		}
		break;
	}
	default:
		break;
	}
	return joinparts(&parts);
}

static char *fielddoc(PosCtx *c, const char *name, size_t n, SymVec *fields) {
	Buf b = {0};
	if (!fields || !fields->n) {
		bufprintf(&b, "No record has a field `%.*s`.", (int)n, name);
		return b.b;
	}
	Buf recs = {0};
	bool first = true;
	for (size_t i = 0; i < fields->n; i++) {
		Node *r = fields->v[i]->node;
		Doc *d = docof(fields->v[i]->file);
		if (!r->record.name)
			continue;
		if (!first)
			bufputc(&recs, '\n');
		first = false;
		bufprintf(&recs, "record %.*s%s", (int)r->record.name->len, ttext(d, r->record.name),
			toklist(d, &r->record.fields));
	}
	bufprintf(&b, "Field `%.*s` of\n\n", (int)n, name);
	codeblock(&b, recs.b ? recs.b : "");
	(void)c;
	return b.b;
}

/* ampkeyword returns the name of the keyword whose "&" is at the
 * position ("&subject", the name directly after it), or NULL. (A
 * position at the end of the "&" is the name's start, which identat
 * finds.) */
static Token *ampkeyword(PosCtx *c) {
	Tokens *ts = &c->d->file->toks;
	size_t lo = 0, hi = ts->n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (ts->v[mid].offset > c->off)
			hi = mid;
		else
			lo = mid + 1;
	}
	if (lo == 0 || lo >= ts->n)
		return NULL;
	Token *t = &ts->v[lo - 1], *n = &ts->v[lo];
	if (t->kind != T_AND || t->implicit || c->off > tend(t) || n->offset != tend(t) || !c->info->iskeyword[lo])
		return NULL;
	return n;
}

static void hover(Server *s, PosCtx *c, Buf *b) {
	if (!c->tok)
		c->tok = ampkeyword(c);
	if (!c->tok) {
		bufputs(b, "null");
		return;
	}
	Target t = target(s, c);
	char *md;
	if (t.isfield)
		md = fielddoc(c, ttext(c->d, c->tok), c->tok->len, t.fields);
	else if (t.sym)
		md = symboldoc(s, t.sym, c);
	else if (t.iskw && t.kw)
		md = keyworddoc(s, t.kw);
	else if (t.iskw) {
		Buf m = {0};
		bufprintf(&m, "`&%.*s` is not a keyword", (int)c->tok->len, ttext(c->d, c->tok));
		md = m.b;
	} else if (t.islink && t.link[0]) {
		Buf m = {0};
		bufprintf(&m, "Links `%s`", t.link);
		md = m.b;
	} else if (t.islink)
		md = "Linked file (source not found; set `ipath` or `$IPATH`)";
	else {
		bufputs(b, "null");
		return;
	}
	bufputs(b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
	json_stringz(b, md);
	bufputs(b, "},\"range\":");
	tokrange(b, c);
	bufputc(b, '}');
}

/* ---- definition, references, highlights */

static void definition(Server *s, PosCtx *c, Buf *b) {
	Target t = target(s, c);
	Buf locs = {0};
	int n = 0;
#define ADD(f, tok)                                                                      \
	do {                                                                             \
		Buf one = {0};                                                           \
		if (location(&one, (f), (tok))) {                                        \
			if (n++)                                                         \
				bufputc(&locs, ',');                                     \
			bufadd(&locs, one.b, one.n);                                     \
		}                                                                        \
	} while (0)
	if (t.isfield) {
		for (size_t i = 0; t.fields && i < t.fields->n; i++)
			ADD(t.fields->v[i]->file, t.fields->v[i]->decl);
	} else if (t.sym && t.sym->kind == S_MACRO) {
		MacroDef *m = t.sym->macro;
		Doc *d;
		if (m->path[0] && (d = load(&s->ws, m->path))) {
			bufputs(&locs, "{\"uri\":");
			json_stringz(&locs, d->uri);
			bufputs(&locs, ",\"range\":");
			jrange(&locs, &d->m, m->offset, m->end);
			bufputc(&locs, '}');
			n++;
		}
	} else if (t.sym && t.sym->kind != S_BUILTIN) {
		if (t.sym->kind == S_GLOBAL) {
			/* A global declared in several places: offer them all. */
			SymVec *g = strmap_get(&c->prog->globals, t.sym->name, strlen(t.sym->name));
			for (size_t i = 0; g && i < g->n; i++)
				ADD(g->v[i]->file, g->v[i]->decl);
		} else
			ADD(t.sym->file, t.sym->decl);
	} else if (t.islink && t.link[0]) {
		bufputs(&locs, "{\"uri\":");
		json_stringz(&locs, pathtouri(t.link));
		bufputs(&locs, ",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}}");
		n++;
	}
#undef ADD
	if (!n) {
		bufputs(b, "null");
		return;
	}
	bufputc(b, '[');
	bufadd(b, locs.b, locs.n);
	bufputc(b, ']');
}

/* ref is an occurrence of a symbol. */
typedef struct Ref {
	Doc *d;
	Token *tok;
	bool isdecl;
} Ref;
typedef VEC(Ref) RefVec;

static bool isglobalkind(SymKind k) { return k == S_GLOBAL || k == S_PROCEDURE || k == S_RECORD; }

static int cmpref(const void *a, const void *b) {
	const Ref *x = a, *y = b;
	int c = strcmp(x->d->uri, y->d->uri);
	if (c)
		return c;
	return x->tok->offset < y->tok->offset ? -1 : x->tok->offset > y->tok->offset;
}

static SemaInfo *infofor(PosCtx *c, Doc *d) { return d == c->d ? c->info : sema_resolve(c->prog, &d->sf); }

static void addref(RefVec *v, Doc *d, Token *t, bool isdecl) {
	Ref r = {d, t, isdecl};
	VPUSH(*v, r);
}

/* refs returns the occurrences of what the identifier at c denotes, in
 * source order within each file. */
static RefVec refs(Server *s, PosCtx *c) {
	Target t = target(s, c);
	RefVec out = {0};
	if (t.sym && sym_is_local(t.sym->kind)) {
		for (size_t k = 0; k < c->d->file->toks.n; k++)
			if (c->info->uses[k] == t.sym) {
				Token *tok = &c->d->file->toks.v[k];
				addref(&out, c->d, tok, tok == t.sym->decl && t.sym->kind != S_IMPLICIT);
			}
	} else if (t.sym && t.sym->kind == S_MACRO) {
		for (size_t i = 0; i < c->docs.n; i++) {
			Doc *d = c->docs.v[i];
			SemaInfo *info = infofor(c, d);
			for (size_t k = 0; k < d->file->toks.n; k++)
				if (info->uses[k] && info->uses[k]->kind == S_MACRO && info->uses[k]->macro == t.sym->macro)
					addref(&out, d, &d->file->toks.v[k], false);
		}
	} else if (t.isfield) {
		const char *name = ttext(c->d, c->tok);
		size_t n = c->tok->len;
		for (size_t i = 0; i < c->docs.n; i++) {
			Doc *d = c->docs.v[i];
			SemaInfo *info = infofor(c, d);
			for (size_t k = 0; k < d->file->toks.n; k++) {
				Token *tok = &d->file->toks.v[k];
				if (info->fielduses[k] && ttexteq(d, tok, name, n))
					addref(&out, d, tok, false);
				if (info->uses[k] && info->uses[k]->kind == S_FIELD && strlen(info->uses[k]->name) == n &&
				    !memcmp(info->uses[k]->name, name, n))
					addref(&out, d, tok, true);
			}
		}
	} else if (t.sym) {
		Symbol *sym = t.sym;
		for (size_t i = 0; i < c->docs.n; i++) {
			Doc *d = c->docs.v[i];
			SemaInfo *info = infofor(c, d);
			for (size_t k = 0; k < d->file->toks.n; k++) {
				Symbol *u = info->uses[k];
				if (u && (u == sym || (isglobalkind(sym->kind) && isglobalkind(u->kind) && !strcmp(u->name, sym->name))))
					addref(&out, d, &d->file->toks.v[k], u->decl == &d->file->toks.v[k]);
			}
		}
	} else if (t.iskw) {
		const char *name = ttext(c->d, c->tok);
		size_t n = c->tok->len;
		for (size_t i = 0; i < c->docs.n; i++) {
			Doc *d = c->docs.v[i];
			SemaInfo *info = infofor(c, d);
			for (size_t k = 0; k < d->file->toks.n; k++)
				if (info->iskeyword[k] && ttexteq(d, &d->file->toks.v[k], name, n))
					addref(&out, d, &d->file->toks.v[k], false);
		}
	}
	qsort(out.v, out.n, sizeof *out.v, cmpref);
	return out;
}

static void references(Server *s, PosCtx *c, bool includedecl, Buf *b) {
	bufputc(b, '[');
	if (c->tok) {
		RefVec rs = refs(s, c);
		int n = 0;
		for (size_t i = 0; i < rs.n; i++) {
			if (rs.v[i].isdecl && !includedecl)
				continue;
			if (n++)
				bufputc(b, ',');
			bufputs(b, "{\"uri\":");
			json_stringz(b, rs.v[i].d->uri);
			bufputs(b, ",\"range\":");
			jrange(b, &rs.v[i].d->m, rs.v[i].tok->offset, tend(rs.v[i].tok));
			bufputc(b, '}');
		}
	}
	bufputc(b, ']');
}

static void highlight(Server *s, PosCtx *c, Buf *b) {
	bufputc(b, '[');
	if (c->tok) {
		RefVec rs = refs(s, c);
		int n = 0;
		for (size_t i = 0; i < rs.n; i++) {
			if (rs.v[i].d != c->d)
				continue;
			if (n++)
				bufputc(b, ',');
			bufputs(b, "{\"range\":");
			jrange(b, &c->d->m, rs.v[i].tok->offset, tend(rs.v[i].tok));
			bufputs(b, ",\"kind\":1}");
		}
	}
	bufputc(b, ']');
}

/* ---- completion */

static const char *const reservedwords[] = {
	"break", "by", "case", "create", "default", "do", "else", "end", "every", "fail", "global", "if",
	"initial", "invocable", "link", "local", "next", "not", "of", "procedure", "record", "repeat",
	"return", "static", "suspend", "then", "to", "until", "while",
};

typedef struct Item {
	char *label, *detail, *doc, *sorttext;
	int kind;
} Item;
typedef VEC(Item) ItemVec;

static void jitem(Buf *b, Item *it) {
	bufputs(b, "{\"label\":");
	json_stringz(b, it->label);
	bufprintf(b, ",\"kind\":%d", it->kind);
	if (it->detail && it->detail[0]) {
		bufputs(b, ",\"detail\":");
		json_stringz(b, it->detail);
	}
	if (it->doc && it->doc[0]) {
		bufputs(b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
		json_stringz(b, it->doc);
		bufputc(b, '}');
	}
	if (it->sorttext && it->sorttext[0]) {
		bufputs(b, ",\"sortText\":");
		json_stringz(b, it->sorttext);
	}
	bufputc(b, '}');
}

static char *cat2(const char *a, const char *z) {
	Buf b = {0};
	bufputs(&b, a);
	bufputs(&b, z);
	return b.b;
}

/* prevtokens returns the last two non-implicit tokens ending at or
 * before off. */
static void prevtokens(File *f, size_t off, Token **prev, Token **before) {
	*prev = *before = NULL;
	size_t i = 0;
	while (i < f->toks.n && !(tend(&f->toks.v[i]) > off || f->toks.v[i].kind == T_EOF))
		i++;
	for (size_t j = i; j-- > 0;) {
		Token *t = &f->toks.v[j];
		if (t->implicit || t->kind == T_EOF)
			continue;
		if (!*prev)
			*prev = t;
		else {
			*before = t;
			return;
		}
	}
}

static Node *enclosingproc(File *f, size_t off) {
	for (size_t i = 0; i < f->decls.n; i++) {
		Node *p = f->decls.v[i];
		if (p->kind != N_PROC)
			continue;
		Token *a = firsttoken(p), *z = lasttoken(p);
		if (a && z && a->offset <= off && (off <= tend(z) || !p->proc.end))
			return p;
	}
	return NULL;
}

static int cmpitem(const void *a, const void *b) {
	return strcmp(((const Item *)a)->sorttext, ((const Item *)b)->sorttext);
}

typedef struct NameCtx {
	CharVec *names;
} NameCtx;

static void collectkey(void *ctx, const char *key, size_t n, void *val) {
	(void)val;
	VPUSH(*((NameCtx *)ctx)->names, xstrndup(key, n));
}

static void identcompletions(PosCtx *c, ItemVec *items) {
	StrMap seen = {0};
#define ADDITEM(it)                                                                      \
	do {                                                                             \
		Item x_ = (it);                                                          \
		if (!strmap_get(&seen, x_.label, strlen(x_.label))) {                    \
			strmap_put(&seen, x_.label, strlen(x_.label), (void *)1);        \
			VPUSH(*items, x_);                                               \
		}                                                                        \
	} while (0)
	for (size_t i = 0; i < c->d->sf.nmacros; i++) {
		MacroScope *m = &c->d->sf.macros[i];
		if (m->from <= c->off && c->off < m->until) {
			Buf det = {0};
			bufprintf(&det, "$define %s %s", m->def->name, m->def->value);
			Item it = {xstrndup(m->def->name, strlen(m->def->name)), trimspace(det.b, det.n), NULL,
				cat2("0", m->def->name), 21};
			ADDITEM(it);
		}
	}
	Node *p = enclosingproc(c->d->file, c->off);
	ProcLocals *pl = p ? sema_locals(c->info, p) : NULL;
	for (size_t i = 0; pl && i < pl->syms.n; i++) {
		Symbol *sym = pl->syms.v[i];
		if (sym->kind == S_IMPLICIT && sym->decl == c->tok)
			continue; /* the word being typed */
		Item it = {xstrndup(sym->name, strlen(sym->name)), xstrndup(sym_kind_name(sym->kind), strlen(sym_kind_name(sym->kind))),
			NULL, cat2("0", sym->name), 6};
		ADDITEM(it);
	}
	CharVec names = {0};
	NameCtx nc = {&names};
	strmap_each(&c->prog->globals, collectkey, &nc);
	for (size_t i = 0; i < names.n; i++) {
		Symbol *sym = sema_global(c->prog, names.v[i], strlen(names.v[i]));
		Item it = {names.v[i], NULL, NULL, cat2("1", names.v[i]), 6};
		if (sym->kind == S_PROCEDURE)
			it.kind = 3, it.detail = cat2("procedure ", procsignature(docof(sym->file), sym->node));
		else if (sym->kind == S_RECORD) {
			Buf det = {0};
			bufprintf(&det, "record %s%s", names.v[i], toklist(docof(sym->file), &sym->node->record.fields));
			it.kind = 22, it.detail = det.b;
		} else
			it.detail = "global";
		ADDITEM(it);
	}
	size_t nf;
	const BuiltinFunc *fs = builtin_functions(&nf);
	for (size_t i = 0; i < nf; i++) {
		Item it = {(char *)fs[i].name, (char *)fs[i].signature, capitalize(fs[i].summary), cat2("2", fs[i].name), 3};
		ADDITEM(it);
	}
	for (size_t i = 0; i < sizeof reservedwords / sizeof reservedwords[0]; i++) {
		Item it = {(char *)reservedwords[i], NULL, NULL, cat2("3", reservedwords[i]), 14};
		ADDITEM(it);
	}
#undef ADDITEM
	qsort(items->v, items->n, sizeof *items->v, cmpitem);
}

static void completion(PosCtx *c, Buf *b) {
	size_t start = c->off;
	if (c->tok && c->tok->offset < c->off)
		start = c->tok->offset; /* completing a partly typed word */
	Token *prev, *before;
	prevtokens(c->d->file, start, &prev, &before);
	ItemVec items = {0};
	if (prev && prev->kind == T_AND && tend(prev) == start) {
		size_t nk;
		const Keyword *ks = builtin_keywords(&nk);
		for (size_t i = 0; i < nk; i++) {
			Item it = {(char *)ks[i].name, cat2("&", ks[i].name), capitalize(ks[i].summary), NULL, 21};
			VPUSH(items, it);
		}
	} else if (prev && prev->kind == T_DOT && tend(prev) == start && before && tok_ender(before->kind)) {
		CharVec names = {0};
		NameCtx nc = {&names};
		strmap_each(&c->prog->fields, collectkey, &nc);
		qsort(names.v, names.n, sizeof *names.v, cmpstr);
		for (size_t i = 0; i < names.n; i++) {
			SymVec *fs = strmap_get(&c->prog->fields, names.v[i], strlen(names.v[i]));
			Buf det = {0};
			bufputs(&det, "field of ");
			bool first = true;
			for (size_t k = 0; fs && k < fs->n; k++) {
				Node *r = fs->v[k]->node;
				if (!r->record.name)
					continue;
				if (!first)
					bufputs(&det, ", ");
				first = false;
				bufadd(&det, ttext(docof(fs->v[k]->file), r->record.name), r->record.name->len);
			}
			Item it = {names.v[i], det.b, NULL, NULL, 5};
			VPUSH(items, it);
		}
	} else
		identcompletions(c, &items);
	bufputs(b, "{\"isIncomplete\":false,\"items\":[");
	for (size_t i = 0; i < items.n; i++) {
		if (i)
			bufputc(b, ',');
		jitem(b, &items.v[i]);
	}
	bufputs(b, "]}");
}

/* ---- signature help */

/* innermostcall returns the innermost call whose argument list
 * contains off: the last found in a pre-order walk, without recursion. */
static Node *innermostcall(File *f, size_t off) {
	Node *best = NULL;
	KidVec stk = {0};
	for (size_t i = f->decls.n; i-- > 0;) {
		KidV k = {NULL, f->decls.v[i]};
		VPUSH(stk, k);
	}
	while (stk.n) {
		KidV k = stk.v[--stk.n];
		if (!k.n)
			continue;
		Node *n = k.n;
		if (n->kind == N_CALL && n->call.open && tend(n->call.open) <= off &&
		    (!n->call.close || off <= n->call.close->offset))
			best = n;
		KidVec ks = {0};
		syn_children(n, kids, &ks);
		for (size_t i = ks.n; i-- > 0;)
			if (ks.v[i].n)
				VPUSH(stk, ks.v[i]);
		free(ks.v);
	}
	free(stk.v);
	return best;
}

static void signaturehelp(PosCtx *c, Buf *b) {
	Node *call = innermostcall(c->d->file, c->off);
	if (!call || !call->call.fun || call->call.fun->kind != N_IDENT || !call->call.fun->ident.name) {
		bufputs(b, "null");
		return;
	}
	Symbol *sym = c->info->uses[tokidx(c->d, call->call.fun->ident.name)];
	if (!sym) {
		bufputs(b, "null");
		return;
	}
	CharVec params = {0};
	char *doc = "";
	if (sym->kind == S_PROCEDURE) {
		Node *p = sym->node;
		Doc *d = docof(sym->file);
		for (size_t i = 0; i < p->proc.params.elems.n; i++)
			if (p->proc.params.elems.v[i])
				VPUSH(params, xstrndup(ttext(d, p->proc.params.elems.v[i]), p->proc.params.elems.v[i]->len));
		if (p->proc.lbrack && params.n)
			params.v[params.n - 1] = cat2(params.v[params.n - 1], "[]");
		doc = doccomment(d, sym);
	} else if (sym->kind == S_RECORD) {
		Doc *d = docof(sym->file);
		TokList *fl = &sym->node->record.fields;
		for (size_t i = 0; i < fl->elems.n; i++)
			if (fl->elems.v[i])
				VPUSH(params, xstrndup(ttext(d, fl->elems.v[i]), fl->elems.v[i]->len));
		doc = "Record constructor";
	} else if (sym->kind == S_BUILTIN) {
		const char *sig = sym->builtin->signature;
		const char *i = strchr(sig, '('), *j = strrchr(sig, ')');
		if (i && j && j > i + 1) {
			const char *p = i + 1;
			while (p <= j) {
				const char *e = p;
				while (e < j && *e != ',')
					e++;
				char *t = trimspace(p, (size_t)(e - p));
				if (t[0])
					VPUSH(params, t);
				p = e + 1;
			}
		}
		doc = capitalize(sym->builtin->summary);
	} else {
		bufputs(b, "null");
		return;
	}
	long active = 0;
	for (size_t i = 0; i < call->call.args.seps.n; i++)
		if (call->call.args.seps.v[i] && call->call.args.seps.v[i]->offset < c->off)
			active++;
	if (params.n && active >= (long)params.n)
		active = (long)params.n - 1; /* extra arguments of a variadic function */
	bufputs(b, "{\"signatures\":[{\"label\":");
	Buf label = {0};
	bufputs(&label, sym->name);
	bufputc(&label, '(');
	for (size_t i = 0; i < params.n; i++) {
		if (i)
			bufputs(&label, ", ");
		bufputs(&label, params.v[i]);
	}
	bufputc(&label, ')');
	json_stringz(b, label.b);
	if (doc[0]) {
		bufputs(b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
		json_stringz(b, doc);
		bufputc(b, '}');
	}
	bufputs(b, ",\"parameters\":[");
	for (size_t i = 0; i < params.n; i++) {
		if (i)
			bufputc(b, ',');
		bufputs(b, "{\"label\":");
		json_stringz(b, params.v[i]);
		bufputc(b, '}');
	}
	bufprintf(b, "]}],\"activeSignature\":0,\"activeParameter\":%ld}", active);
}

/* ---- rename */

static bool failwith(int *code, char **err, Buf *m) {
	*code = CodeRequestFailed, *err = m->b;
	return false;
}

static bool renameable(Server *s, PosCtx *c, Target *t, int *code, char **err) {
	*t = target(s, c);
	if (!c->tok || !t->sym || t->sym->kind == S_BUILTIN || t->sym->kind == S_MACRO || t->isfield) {
		Buf m = {0};
		bufputs(&m, "nothing here that can be renamed");
		return failwith(code, err, &m);
	}
	/* Don't edit files outside the document's directory (libraries). */
	char *dir = c->d->path[0] ? dirname_(c->d->path) : xstrndup(".", 1);
	RefVec rs = refs(s, c);
	for (size_t i = 0; i < rs.n; i++) {
		Doc *rd = rs.v[i].d;
		if (!rd->path[0] || strcmp(dirname_(rd->path), dir)) {
			Buf m = {0};
			goquote(&m, ttext(c->d, c->tok), c->tok->len);
			bufprintf(&m, " is also used in %s, outside this directory", relpath(c->d, rd));
			return failwith(code, err, &m);
		}
	}
	return true;
}

static void preparerename(Server *s, PosCtx *c, Buf *b, int *code, char **err) {
	Target t;
	if (!renameable(s, c, &t, code, err))
		return;
	bufputs(b, "{\"range\":");
	tokrange(b, c);
	bufputs(b, ",\"placeholder\":");
	json_string(b, ttext(c->d, c->tok), c->tok->len);
	bufputc(b, '}');
}

/* proctokens gives the token index range [lo, hi] of procedure p. */
static bool procrange(Doc *d, Node *p, size_t *lo, size_t *hi) {
	Token *a = firsttoken(p), *z = lasttoken(p);
	if (!a || !z)
		return false;
	*lo = tokidx(d, a), *hi = tokidx(d, z);
	return true;
}

static bool renameconflict(Server *s, PosCtx *c, Symbol *sym, const char *newname, int *code, char **err) {
	(void)s;
	size_t nn = strlen(newname);
	Buf m = {0};
	if (sym_is_local(sym->kind)) {
		Node *p = sym->node;
		ProcLocals *pl = sema_locals(c->info, p);
		Symbol *other = pl ? strmap_get(&pl->byname, newname, nn) : NULL;
		if (other) {
			goquotez(&m, newname);
			bufprintf(&m, " is already a %s of this procedure", sym_kind_name(other->kind));
			return failwith(code, err, &m);
		}
		/* Would capture a use of a global or built-in of that name. */
		size_t lo, hi;
		if (procrange(c->d, p, &lo, &hi))
			for (size_t k = lo; k <= hi; k++) {
				Symbol *u = c->info->uses[k];
				if (u && !strcmp(u->name, newname)) {
					goquotez(&m, newname);
					bufprintf(&m, " is used in this procedure as a %s", sym_kind_name(u->kind));
					return failwith(code, err, &m);
				}
			}
		return true;
	}
	SymVec *g = strmap_get(&c->prog->globals, newname, nn);
	if (g && g->n) {
		goquotez(&m, newname);
		bufprintf(&m, " is already a %s (%s)", sym_kind_name(g->v[0]->kind), relpath(c->d, docof(g->v[0]->file)));
		return failwith(code, err, &m);
	}
	/* Each procedure using the global must not have a local of the new
	 * name, nor use a built-in of that name. */
	for (size_t i = 0; i < c->docs.n; i++) {
		Doc *d = c->docs.v[i];
		SemaInfo *info = infofor(c, d);
		for (size_t j = 0; j < d->file->decls.n; j++) {
			Node *p = d->file->decls.v[j];
			size_t lo, hi;
			if (p->kind != N_PROC || !procrange(d, p, &lo, &hi))
				continue;
			bool usessym = false, usesnew = false;
			for (size_t k = lo; k <= hi; k++) {
				Symbol *u = info->uses[k];
				if (u) {
					usessym = usessym || (!strcmp(u->name, sym->name) && isglobalkind(u->kind));
					usesnew = usesnew || !strcmp(u->name, newname);
				}
			}
			if (usessym && usesnew && p->proc.name) {
				goquotez(&m, newname);
				bufprintf(&m, " is already used in procedure %.*s", (int)p->proc.name->len, ttext(d, p->proc.name));
				return failwith(code, err, &m);
			}
		}
	}
	if (builtin_function(newname, nn)) {
		/* A new global of this name would hide the built-in wherever
		 * it is used. */
		for (size_t i = 0; i < c->docs.n; i++) {
			Doc *d = c->docs.v[i];
			SemaInfo *info = infofor(c, d);
			for (size_t k = 0; k < d->file->toks.n; k++) {
				Symbol *u = info->uses[k];
				if (u && u->kind == S_BUILTIN && !strcmp(u->name, newname)) {
					goquotez(&m, newname);
					bufprintf(&m, " would hide the built-in function used in %s", relpath(c->d, d));
					return failwith(code, err, &m);
				}
			}
		}
	}
	return true;
}

static void renamesym(Server *s, PosCtx *c, const char *newname, Buf *b, int *code, char **err) {
	Target t;
	if (!renameable(s, c, &t, code, err))
		return;
	Tokens ts = lex_tokenize(newname, strlen(newname));
	if (ts.n != 2 || ts.v[0].kind != T_IDENT || ts.v[0].len != strlen(newname)) {
		Buf m = {0};
		goquotez(&m, newname);
		bufputs(&m, " is not a valid identifier");
		*code = CodeInvalidParams, *err = m.b;
		return;
	}
	Symbol *sym = t.sym;
	if (!strcmp(newname, sym->name)) {
		bufputs(b, "{\"changes\":{}}");
		return;
	}
	RefVec rs = refs(s, c);
	if (!renameconflict(s, c, sym, newname, code, err))
		return;
	/* edits by URI, the URIs sorted (as Go writes a map) */
	CharVec uris = {0};
	for (size_t i = 0; i < rs.n; i++) {
		bool have = false;
		for (size_t k = 0; k < uris.n; k++)
			if (!strcmp(uris.v[k], rs.v[i].d->uri))
				have = true;
		if (!have)
			VPUSH(uris, rs.v[i].d->uri);
	}
	qsort(uris.v, uris.n, sizeof *uris.v, cmpstr);
	bufputs(b, "{\"changes\":{");
	for (size_t k = 0; k < uris.n; k++) {
		if (k)
			bufputc(b, ',');
		json_stringz(b, uris.v[k]);
		bufputs(b, ":[");
		int n = 0;
		for (size_t i = 0; i < rs.n; i++) {
			if (strcmp(rs.v[i].d->uri, uris.v[k]))
				continue;
			if (n++)
				bufputc(b, ',');
			bufputs(b, "{\"range\":");
			jrange(b, &rs.v[i].d->m, rs.v[i].tok->offset, tend(rs.v[i].tok));
			bufputs(b, ",\"newText\":");
			json_stringz(b, newname);
			bufputc(b, '}');
		}
		bufputc(b, ']');
	}
	bufputs(b, "}}");
}

/* ---- workspace symbols */

static bool containsfold(const char *s, const char *q) {
	size_t n = strlen(s), m = strlen(q);
	for (size_t i = 0; i + m <= n; i++) {
		size_t k = 0;
		while (k < m && ((s[i + k] >= 'A' && s[i + k] <= 'Z') ? s[i + k] + 32 : s[i + k]) == q[k])
			k++;
		if (k == m)
			return true;
	}
	return false;
}

static void workspacesymbols(Server *s, const char *query, Buf *b) {
	char *q = xstrndup(query, strlen(query));
	for (char *p = q; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p + 32);
	StrMap seen = {0};
	DocVec open = opendocs(s);
	bufputc(b, '[');
	int n = 0;
	for (size_t i = 0; i < open.n; i++) {
		DocVec order = {0};
		program(&s->ws, open.v[i], &order);
		for (size_t j = 0; j < order.n; j++) {
			Doc *d = order.v[j];
			if (strmap_get(&seen, d->uri, strlen(d->uri)))
				continue;
			strmap_put(&seen, d->uri, strlen(d->uri), (void *)1);
			DSymVec syms = docsymbols(d);
			for (size_t k = 0; k < syms.n; k++) {
				if (syms.v[k].kind == SymbolModule || !containsfold(syms.v[k].name, q))
					continue;
				if (n++)
					bufputc(b, ',');
				jsyminfo(b, syms.v[k].name, syms.v[k].kind, d->uri, &d->m, syms.v[k].sa, syms.v[k].sz,
					d->path[0] ? basename_(d->path) : ".");
			}
		}
	}
	bufputc(b, ']');
}

/* ---- dispatch */

static bool lsp_feature(Server *s, const char *method, JVal *params, Buf *b, int *code, char **err) {
	static const char *const positional[] = {
		"textDocument/hover", "textDocument/definition", "textDocument/references",
		"textDocument/documentHighlight", "textDocument/completion", "textDocument/signatureHelp",
		"textDocument/prepareRename", "textDocument/rename",
	};
	if (!strcmp(method, "workspace/symbol")) {
		workspacesymbols(s, json_str(params, "query"), b);
		return true;
	}
	int which = -1;
	for (int i = 0; i < 8; i++)
		if (!strcmp(method, positional[i]))
			which = i;
	if (which < 0)
		return false;
	PosCtx c;
	if (!at(s, params, &c, code, err))
		return true;
	switch (which) {
	case 0: hover(s, &c, b); break;
	case 1: definition(s, &c, b); break;
	case 2: references(s, &c, json_bool(params, "context.includeDeclaration"), b); break;
	case 3: highlight(s, &c, b); break;
	case 4: completion(&c, b); break;
	case 5: signaturehelp(&c, b); break;
	case 6: preparerename(s, &c, b, code, err); break;
	case 7: renamesym(s, &c, json_str(params, "newName"), b, code, err); break;
	}
	return true;
}

static int lsp_fetchdocs(const char *dir) { return refdocs_fetch(dir); }

int lsp_main(int argc, char **argv) {
	const char *logfile = NULL, *docs = NULL;
	if (argc > 1 && !strcmp(argv[1], "fetch-docs")) {
		const char *dir = cachedir();
		for (int i = 2; i < argc; i++) {
			if ((!strcmp(argv[i], "-dir") || !strcmp(argv[i], "--dir")) && i + 1 < argc)
				dir = argv[++i];
			else if (!strncmp(argv[i], "-dir=", 5))
				dir = argv[i] + 5;
			else if (help_isflag(argv[i]))
				help_page("lsp");
			else
				help_usage("lsp");
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
		} else if (help_isflag(argv[i]))
			help_page("lsp");
		else
			help_usage("lsp");
	}
	Server s = {0};
	s.in = stdin, s.out = stdout, s.logf = stderr;
	if (logfile && !(s.logf = fopen(logfile, "a"))) {
		fprintf(stderr, "gcon lsp: open %s: %s\n", logfile, strerror(errno));
		return 2;
	}
	s.docsdir = docs ? docs : cachedir();
	s.refdocs = refdocs_load(s.docsdir);
	s.ws.root = root_find().dir;
	return serve(&s);
}
