/*
 * The driver (driver.h), as the Go reference's internal/run (run.go,
 * build.go, gconvm.go, exec_unix.go) and internal/root: gcon's
 * infrastructure, which is gcon's own design, not icont's
 * (docs/design.md, "the driver and infrastructure are gcon's").
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "driver.h"

#include "compile.h"
#include "ipp.h"
#include "lit.h"
#include "sema.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char **environ;

/* ------------------------------------------------------------------ */
/* Paths */

/* pathclean is Go's filepath.Clean: the shortest path equivalent to p
 * by lexical processing. */
char *pathclean(const char *p) {
	if (!*p)
		return xstrndup(".", 1);
	bool rooted = p[0] == '/';
	size_t n = strlen(p);
	char *out = xmalloc(n + 2);
	size_t w = 0, r = 0, dotdot = 0;
	if (rooted) {
		out[w++] = '/';
		r = 1;
		dotdot = 1;
	}
	while (r < n) {
		if (p[r] == '/')
			r++;
		else if (p[r] == '.' && (r + 1 == n || p[r + 1] == '/'))
			r++;
		else if (p[r] == '.' && p[r + 1] == '.' && (r + 2 == n || p[r + 2] == '/')) {
			r += 2;
			if (w > dotdot) {
				w--;
				while (w > dotdot && out[w] != '/')
					w--;
			} else if (!rooted) {
				if (w > 0)
					out[w++] = '/';
				out[w++] = '.';
				out[w++] = '.';
				dotdot = w;
			}
		} else {
			if ((rooted && w != 1) || (!rooted && w != 0))
				out[w++] = '/';
			for (; r < n && p[r] != '/'; r++)
				out[w++] = p[r];
		}
	}
	if (w == 0)
		out[w++] = '.';
	out[w] = 0;
	return out;
}

char *pathjoin(const char *dir, const char *name) {
	if (!*dir)
		return pathclean(name);
	Buf b = {0};
	bufputs(&b, dir);
	bufputc(&b, '/');
	bufputs(&b, name);
	char *c = pathclean(b.b);
	free(b.b);
	return c;
}

char *pathabs(const char *p) {
	if (p[0] == '/')
		return pathclean(p);
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd))
		return pathclean(p);
	return pathjoin(cwd, p);
}

/* executable returns the path of the running gcon, as Go's
 * os.Executable: absolute, symbolic links not followed (on Linux the
 * kernel's /proc/self/exe has them followed). */
char *executable(const char *argv0) {
#if defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(NULL, &size);
	char *buf = xmalloc(size + 1);
	if (_NSGetExecutablePath(buf, &size) == 0)
		return pathabs(buf);
#elif defined(__linux__)
	char buf[PATH_MAX];
	ssize_t k = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (k > 0) {
		buf[k] = 0;
		return xstrndup(buf, (size_t)k);
	}
#endif
	if (strchr(argv0, '/'))
		return pathabs(argv0);
	const char *path = getenv("PATH");
	while (path && *path) {
		const char *e = strchr(path, ':');
		size_t n = e ? (size_t)(e - path) : strlen(path);
		char *dir = xstrndup(n ? path : ".", n ? n : 1);
		char *f = pathjoin(dir, argv0);
		if (access(f, X_OK) == 0)
			return pathabs(f);
		path = e ? e + 1 : NULL;
	}
	return xstrndup("gcon", 4);
}

/* errtext is strerror as Go's error text has it: lower case. */
static const char *errtext(int e) {
	static char buf[256];
	snprintf(buf, sizeof buf, "%s", strerror(e));
	if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z'))
		buf[0] = (char)(buf[0] - 'A' + 'a');
	return buf;
}

static bool isdir(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool isfile(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool readfile(const char *name, Buf *b) {
	FILE *f = fopen(name, "rb");
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

/* ------------------------------------------------------------------ */
/* The root */

bool root_is(const char *dir) { return isdir(pathjoin(dir, "lib/ipl/procs")); }

Root root_find(void) {
	Root r = {NULL, "not found"};
	const char *env = getenv("GCONROOT");
	if (env && *env) {
		r.dir = env;
		r.source = "GCONROOT";
		return r;
	}
	char *exe = executable("gcon");
	char real[PATH_MAX];
	if (realpath(exe, real)) {
		char *d = pathjoin(real, "../..");
		if (root_is(d)) {
			r.dir = d;
			r.source = "executable";
			return r;
		}
	}
	if (GCON_DEFAULT_ROOT[0] && root_is(GCON_DEFAULT_ROOT)) {
		r.dir = GCON_DEFAULT_ROOT;
		r.source = "default";
	}
	return r;
}

/* envpath splits the directories of a search-path variable, which may
 * be separated by spaces or colons, as icont's are. */
static StrVec envpath(const char *name) {
	StrVec v = {0};
	const char *s = getenv(name);
	while (s && *s) {
		while (*s == ' ' || *s == ':')
			s++;
		const char *e = s;
		while (*e && *e != ' ' && *e != ':')
			e++;
		if (e > s)
			VPUSH(v, xstrndup(s, (size_t)(e - s)));
		s = e;
	}
	return v;
}

static StrVec searchpath(const char *var, const char *root, const char *a, const char *b) {
	StrVec v = envpath(var);
	if (root) {
		VPUSH(v, pathjoin(root, a));
		VPUSH(v, pathjoin(root, b));
	}
	return v;
}

StrVec cfg_libpath(const Config *c) { return searchpath("IPATH", c->root, "lib/ipl/procs", "lib/ipl/gprocs"); }

StrVec cfg_includepath(const Config *c) {
	return searchpath("LPATH", c->root, "lib/ipl/incl", "lib/ipl/gincl");
}

/* ------------------------------------------------------------------ */
/* Translation */

/* The predefined symbols: _V9, and one for each of gcon-vm's &features
 * that has one, each defined as 1, as icont's are. */
static const char *const predefined[] = {
	"_V9", "_UNIX",
#if defined(__APPLE__)
	"_MACINTOSH",
#endif
	"_ASCII", "_CO_EXPRESSIONS", "_DYNAMIC_LOADING", "_EXTERNAL_VALUES",
	"_KEYBOARD_FUNCTIONS", "_LARGE_INTEGERS", "_PIPES", "_SYSTEM_FUNCTION", "_GRAPHICS",
	/* the display server is icon-sdl's SDL backend; its keys are X
	 * keysyms, which the IPL's keysyms.icn (as tools/ipl/patch.sh
	 * changes it) names under _X_KEYSYMS, defined by this */
	"_SDL_GRAPHICS",
};
enum { NPredefined = sizeof predefined / sizeof predefined[0] };

static const char *const ones[NPredefined] = {
	"1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1",
#if defined(__APPLE__)
	"1",
#endif
};

/* A source file of the program. */
typedef struct Unit {
	char *path; /* where it was read from */
	char *name; /* its name in the program: path for the main file, the link name + ".icn" for others */
	File *file;
	PPResult *pp;
	StrVec errs; /* its preprocessing and syntax errors */
} Unit;

static char *errline(const char *file, long line, const char *msg) {
	Buf b = {0};
	bufprintf(&b, "File %s; Line %ld # %s", file, line, msg);
	return b.b;
}

/* load preprocesses and parses the source of a file known as name.
 * Each file is preprocessed on its own, from the predefined symbols, as
 * icont translates each file on its own. */
static Unit *load(const char *path, const char *name, const char *src, size_t n, const StrVec *incpath) {
	Unit *u = xcalloc(1, sizeof *u);
	u->path = xstrndup(path, strlen(path));
	u->name = xstrndup(name, strlen(name));
	PPOptions o = {predefined, ones, NPredefined, incpath->v, incpath->n};
	u->pp = ipp_preprocess(u->name, src, n, &o);
	for (size_t i = 0; i < u->pp->errs.n; i++)
		VPUSH(u->errs, ipp_error(&u->pp->errs.v[i]));
	u->file = syn_parse(u->pp->src.b, u->pp->src.n);
	for (size_t i = 0; i < u->file->errs.n; i++) {
		const char *file;
		long line;
		ipp_locate(&u->pp->map, u->file->errs.v[i].offset, &file, &line);
		VPUSH(u->errs, errline(file, line, u->file->errs.v[i].msg));
	}
	return u;
}

/* hasmain reports whether u declares a procedure main. */
static bool hasmain(Unit *u) {
	for (size_t i = 0; i < u->file->decls.n; i++) {
		Node *d = u->file->decls.v[i];
		if (d->kind == N_PROC && d->proc.name && d->proc.name->len == 4 &&
		    !memcmp(u->file->src + d->proc.name->offset, "main", 4))
			return true;
	}
	return false;
}

/* findlink returns the candidates for "link name", in search order:
 * name.icn in the current directory, then in each directory of the
 * library path, as icont's pathfind searches for name.u1: each
 * directory, with a "/" added if it lacks one, and the last component of
 * the name (common/filepart.c). */
static StrVec findlink(const char *name, const StrVec *libpath) {
	StrVec c = {0};
	Buf file = {0};
	bufputs(&file, name);
	bufputs(&file, ".icn");
	if (isfile(file.b))
		VPUSH(c, file.b);
	const char *base = strrchr(file.b, '/');
	base = base ? base + 1 : file.b;
	for (size_t i = 0; i < libpath->n; i++) {
		const char *dir = libpath->v[i];
		size_t dn = strlen(dir);
		if (!dn)
			continue;
		Buf p = {0};
		bufputs(&p, dir);
		if (dir[dn - 1] != '/')
			bufputc(&p, '/');
		bufputs(&p, base);
		if (isfile(p.b))
			VPUSH(c, p.b);
	}
	return c;
}

Front *front_end(const char *path, const char *src, size_t n, const Config *c, StrVec *errs) {
	StrVec libpath = cfg_libpath(c), incpath = cfg_includepath(c);
	VEC(Unit *) units = {0};
	VPUSH(units, load(path, path, src, n, &incpath));
	StrMap seen = {0};
	char *ap = pathabs(path);
	strmap_put(&seen, ap, strlen(ap), (void *)1);
	for (size_t i = 0; i < units.n; i++) {
		Unit *u = units.v[i];
		for (size_t k = 0; k < u->errs.n; k++)
			VPUSH(*errs, u->errs.v[k]);
		for (size_t j = 0; j < u->file->decls.n; j++) {
			Node *d = u->file->decls.v[j];
			if (d->kind != N_LINK)
				continue;
			for (size_t k = 0; k < d->link.files.elems.n; k++) {
				Token *t = d->link.files.elems.v[k];
				if (!t)
					continue;
				char *name = xstrndup(u->file->src + t->offset, t->len);
				if (t->kind == T_STRINGLIT) {
					char *s;
					size_t sn;
					if (!lit_string(u->file->src + t->offset, t->len, &s, &sn))
						name = xstrndup(s, sn);
					else
						name = "";
				}
				/* The first file found that is not a program: a file
				 * declaring main cannot be linked into a program (it
				 * would redeclare main), so the search passes over it,
				 * as icont passes over a source file that was never
				 * translated to ucode. */
				StrVec cands = findlink(name, &libpath);
				Unit *found = NULL;
				bool already = false;
				for (size_t m = 0; m < cands.n; m++) {
					char *ac = pathabs(cands.v[m]);
					if (strmap_get(&seen, ac, strlen(ac))) {
						already = true; /* already part of the program */
						break;
					}
					Buf b = {0};
					if (!readfile(cands.v[m], &b))
						continue;
					Buf uname = {0};
					bufputs(&uname, name);
					bufputs(&uname, ".icn");
					Unit *cu = load(cands.v[m], uname.b, b.b, b.n, &incpath);
					if (!hasmain(cu)) {
						found = cu;
						break;
					}
				}
				if (!found && !already) {
					const char *file;
					long line;
					ipp_locate(&u->pp->map, t->offset, &file, &line);
					Buf m = {0};
					bufprintf(&m, "cannot resolve reference to file '%s.icn'", name);
					VPUSH(*errs, errline(file, line, m.b));
				} else if (found) {
					char *af = pathabs(found->path);
					strmap_put(&seen, af, strlen(af), (void *)1);
					VPUSH(units, found);
				}
			}
		}
	}
	if (errs->n)
		return NULL;
	SemaFile **files = xcalloc(units.n, sizeof *files);
	for (size_t i = 0; i < units.n; i++) {
		files[i] = xcalloc(1, sizeof *files[i]);
		files[i]->path = units.v[i]->name;
		files[i]->syn = units.v[i]->file;
		files[i]->preprocessed = true;
	}
	Program *p = sema_program(files, units.n, true);
	CompileFile *cfs = xcalloc(units.n, sizeof *cfs);
	for (size_t i = 0; i < units.n; i++) {
		cfs[i].info = sema_resolve(p, files[i]);
		cfs[i].map = &units.v[i]->pp->map;
		for (size_t k = 0; k < cfs[i].info->diags.n; k++) {
			Diagnostic *d = &cfs[i].info->diags.v[k];
			if (d->severity != SEV_ERROR)
				continue;
			const char *file;
			long line;
			ipp_locate(&units.v[i]->pp->map, d->offset, &file, &line);
			VPUSH(*errs, errline(file, line, d->msg));
		}
	}
	if (errs->n)
		return NULL;
	Front *f = xcalloc(1, sizeof *f);
	f->prog = p, f->files = cfs, f->n = units.n;
	f->paths = xcalloc(units.n, sizeof *f->paths);
	for (size_t i = 0; i < units.n; i++)
		f->paths[i] = units.v[i]->path;
	return f;
}

Prog *translate(const char *path, const char *src, size_t n, const Config *c, StrVec *errs) {
	Front *f = front_end(path, src, n, c, errs);
	if (!f)
		return NULL;
	Prog *prog = compile_program(f->prog, f->files, f->n, errs);
	return errs->n ? NULL : prog;
}

/* ------------------------------------------------------------------ */
/* Built programs: a shell script that runs gcon-vm (or gcon) on
 * itself, then the encoded program (docs/design.md, "Decision: the
 * built program is an icont-style script"). */

/* marker ends the shell header, as "[executable Icon binary follows]"
 * ends icont's; the encoded program follows it at once. */
static const char marker[] = "[gcon program follows]\n";

static int cmpstrs(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void buildinfo(const char ***keys, const char ***vals, size_t *n) {
	static const char *k[2] = {"gcon", "predefined"};
	static const char *v[2];
	if (!v[0]) {
		v[0] = GCON_VERSION;
		const char *syms[NPredefined];
		for (int i = 0; i < NPredefined; i++)
			syms[i] = predefined[i];
		qsort(syms, NPredefined, sizeof *syms, cmpstrs);
		Buf b = {0};
		for (int i = 0; i < NPredefined; i++) {
			if (i)
				bufputc(&b, ' ');
			bufprintf(&b, "%s=1", syms[i]);
		}
		v[1] = b.b;
	}
	*keys = k, *vals = v, *n = 2;
}

/* shellquote quotes s for sh. */
static void shellquote(Buf *b, const char *s) {
	bufputc(b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			bufputs(b, "'\\''");
		else
			bufputc(b, *s);
	}
	bufputc(b, '\'');
}

/* header returns the shell header of a built program. It runs the
 * program on gcon-vm directly where it can, so as not to start gcon, and
 * otherwise through gcon run. In order: $GCONVM, a gcon-vm; $GCON (which
 * must then work), or gcon beside the program; vmpath, the gcon-vm of the
 * root of the gcon that built it; the gcon that built it, gconpath; and
 * gcon on PATH. */
static void header(Buf *b, const char *gconpath, const char *vmpath) {
	bufputs(b, "#!/bin/sh\nGCBIN=");
	shellquote(b, gconpath);
	bufputs(b, "\nGCVM=");
	shellquote(b, vmpath);
	bufputs(b, "\n"
		   "case \"$0\" in */*) GCLCL=\"${0%/*}/gcon\" ;; *) GCLCL=gcon ;; esac\n"
		   "[ -n \"$GCONVM\" ] && exec \"$GCONVM\" \"$0\" \"$@\"\n"
		   "[ -n \"$GCON\" ] && exec \"$GCON\" run \"$0\" \"$@\"\n"
		   "[ -x \"$GCLCL\" ] && exec \"$GCLCL\" run \"$0\" \"$@\"\n"
		   "[ -x \"$GCVM\" ] && exec \"$GCVM\" \"$0\" \"$@\"\n"
		   "[ -x \"$GCBIN\" ] && exec \"$GCBIN\" run \"$0\" \"$@\"\n"
		   "exec gcon run \"$0\" \"$@\"\n");
	bufputs(b, marker);
}

const char *built_encoding(const char *data, size_t n, size_t *len) {
	if (n < 2 || memcmp(data, "#!", 2) != 0)
		return NULL;
	Buf pat = {0};
	bufputc(&pat, '\n');
	bufputs(&pat, marker);
	bufadd(&pat, FORMAT_MAGIC, 8);
	for (size_t i = 0; i + pat.n <= n; i++)
		if (!memcmp(data + i, pat.b, pat.n)) {
			size_t start = i + 1 + strlen(marker);
			*len = n - start;
			return data + start;
		}
	return NULL;
}

bool buildfile(const char *path, const char *out, const char *self, const Config *c, StrVec *errs) {
	Buf src = {0};
	if (!readfile(path, &src)) {
		Buf m = {0};
		bufprintf(&m, "open %s: %s", path, errtext(errno));
		VPUSH(*errs, m.b);
		return false;
	}
	Prog *prog = translate(path, src.b, src.n, c, errs);
	if (!prog)
		return false;
	const char **k, **v;
	size_t ni;
	buildinfo(&k, &v, &ni);
	Buf enc = {0};
	const char *err = encode(prog, k, v, ni, &enc);
	if (err) {
		VPUSH(*errs, err);
		return false;
	}
	Buf b = {0};
	header(&b, self, c->root ? pathjoin(c->root, "libexec/gcon-vm") : "");
	bufadd(&b, enc.b, enc.n);
	int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (fd < 0 || write(fd, b.b, b.n) != (ssize_t)b.n || close(fd) != 0) {
		Buf m = {0};
		bufprintf(&m, "open %s: %s", out, errtext(errno));
		VPUSH(*errs, m.b);
		return false;
	}
	/* An existing file without execute bits gets one wherever it is
	 * readable, as icont's setexe does. */
	struct stat st;
	if (stat(out, &st) == 0) {
		mode_t m = st.st_mode & 0777;
		if (!(m & 0111))
			chmod(out, m | (m & 0444) >> 2);
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Running on gcon-vm */

const char *findvm(const char *root, const char **from) {
	const char *v = getenv("GCONVM");
	if (v && *v) {
		*from = "GCONVM";
		return v;
	}
	if (root) {
		char *p = pathjoin(root, "libexec/gcon-vm");
		struct stat st;
		if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
			*from = "root";
			return p;
		}
	}
	*from = "not found";
	return NULL;
}

/* progname is &progname for source run as path (the Go's vm.ProgName):
 * the path without ".icn", made to name a file ("./x" for "x"). */
static char *progname(const char *path) {
	size_t n = strlen(path);
	if (n >= 4 && !strcmp(path + n - 4, ".icn"))
		n -= 4;
	Buf b = {0};
	if (!memchr(path, '/', n))
		bufputs(&b, "./");
	bufadd(&b, path, n);
	return b.b;
}

/* tempprogram writes enc to a file deleted at once, returning a
 * descriptor of it that an exec'd process inherits, at offset 0. */
static int tempprogram(const char *enc, size_t n) {
	const char *dir = getenv("TMPDIR");
	Buf t = {0};
	bufputs(&t, dir && *dir ? dir : "/tmp");
	bufputs(&t, "/gcon-XXXXXX.prog");
	int fd = mkstemps(t.b, 5);
	if (fd < 0)
		return -1;
	unlink(t.b);
	if (write(fd, enc, n) != (ssize_t)n || lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int execvm(const char *vm, const char *path, char **args, int nargs, bool dis, const Config *c) {
	Buf data = {0};
	if (!readfile(path, &data)) {
		fprintf(stderr, "open %s: %s\n", path, errtext(errno));
		return 1;
	}
	VEC(char *) argv = {0};
	VPUSH(argv, (char *)vm);
	if (dis)
		VPUSH(argv, "-dis");
	size_t len;
	if (built_encoding(data.b, data.n, &len))
		VPUSH(argv, (char *)path);
	else {
		StrVec errs = {0};
		Prog *prog = translate(path, data.b, data.n, c, &errs);
		if (!prog) {
			report(&errs, c);
			return 1;
		}
		/* A listing of source has no build information, as the Go's. */
		const char **k = NULL, **v = NULL;
		size_t ni = 0;
		if (!dis)
			buildinfo(&k, &v, &ni);
		Buf enc = {0};
		const char *err = encode(prog, k, v, ni, &enc);
		if (err) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		int fd = tempprogram(enc.b, enc.n);
		if (fd < 0) {
			fprintf(stderr, "gcon: %s\n", strerror(errno));
			return 1;
		}
		if (!dis) {
			VPUSH(argv, "-progname");
			VPUSH(argv, progname(path));
		}
		Buf fdpath = {0};
		bufprintf(&fdpath, "/dev/fd/%d", fd);
		VPUSH(argv, fdpath.b);
	}
	for (int i = 0; i < nargs; i++)
		VPUSH(argv, args[i]);
	VPUSH(argv, NULL);
	execve(vm, argv.v, environ);
	fprintf(stderr, "gcon: cannot run %s: %s\n", vm, strerror(errno));
	return 1;
}

void report(const StrVec *errs, const Config *c) {
	bool unresolved = false;
	for (size_t i = 0; i < errs->n; i++) {
		fprintf(stderr, "%s\n", errs->v[i]);
		unresolved = unresolved || strstr(errs->v[i], "cannot resolve reference to file");
	}
	if (unresolved && !c->root)
		fprintf(stderr, "gcon: no GCONROOT found, so the Icon Program Library is not searched; see gcon env\n");
}
