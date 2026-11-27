/*
 * The driver (the Go reference's internal/run and internal/root):
 * gcon's root, the search paths, translating a program with the files
 * it links, building it, and running it on gcon-vm (driver.c).
 */
#ifndef GCON_DRIVER_H
#define GCON_DRIVER_H

#include "compile.h"
#include "prog.h"

/* gcon's version, stamped by build.sh as the Go's module version is: a
 * pseudo-version naming the commit, "+dirty" for uncommitted changes. */
#ifndef GCON_VERSION
#define GCON_VERSION "(devel)"
#endif

/* The root a packager names at build time (-DGCON_DEFAULT_ROOT=...). */
#ifndef GCON_DEFAULT_ROOT
#define GCON_DEFAULT_ROOT ""
#endif

/* A gcon root, found as Go finds GOROOT: $GCONROOT if set (used as it
 * is); else the directory above the executable, symbolic links
 * followed, if it holds the library; else GCON_DEFAULT_ROOT, if it does.
 * (The Go's last resort, Go's module cache, is for a gcon installed by
 * go install.) */
typedef struct Root {
	const char *dir;    /* NULL if not found */
	const char *source; /* "GCONROOT", "executable", "default", "not found" */
} Root;

Root root_find(void);
bool root_is(const char *dir); /* dir holds the library */

/* The configuration for translating. */
typedef struct Config {
	const char *root; /* gcon's root, or NULL */
} Config;

/* The directories link and $include search after the current
 * directory: IPATH (LPATH), then the root's library (include files). */
StrVec cfg_libpath(const Config *c);
StrVec cfg_includepath(const Config *c);

/* front_end reads, preprocesses, parses and resolves the program whose
 * main file is path (n bytes of source src) and the files it links,
 * transitively: its program and, for each file, its resolution and
 * preprocessor map; NULL, with errors in errs, if there are some. */
/* LinkRes: how a link declaration's file resolved (vet's VetLinkKind:
 * 1 the program's own main file, 2 a file of the root's lib/gcon). */
typedef struct LinkRes {
	Token *tok;
	int kind;
} LinkRes;

typedef struct Front {
	Program *prog;
	CompileFile *files;
	char **paths; /* where each file was read from */
	size_t n;
	LinkRes *links; /* each link declaration's resolution, when not an ordinary file */
	size_t nlinks;
} Front;
Front *front_end(const char *path, const char *src, size_t n, const Config *c, StrVec *errs);

/* translate translates the program whose main file is path (n bytes of
 * source src) and the files it links, transitively; NULL, with errors
 * in errs, if there are some. */
Prog *translate(const char *path, const char *src, size_t n, const Config *c, StrVec *errs);

/* buildinfo gives the build information recorded in a built program:
 * gcon's version and the predefined symbols. */
void buildinfo(const char ***keys, const char ***vals, size_t *n);

/* encoded returns the encoded program within data if data is a built
 * program (a file starting "#!" in which a line holding the marker is
 * followed by the magic), and its length; else NULL. */
const char *built_encoding(const char *data, size_t n, size_t *len);

/* buildfile translates the program whose main file is path and writes
 * it, built, to out, made executable; self is the gcon building it.
 * Errors go to errs. */
bool buildfile(const char *path, const char *out, const char *self, const Config *c, StrVec *errs);

/* findvm gives the gcon-vm to run programs, and how it was chosen. */
const char *findvm(const char *root, const char **from);

/* execvm runs the program at path (built, or source translated into a
 * file deleted at once) on vm with args (dis: gcon-vm -dis instead),
 * replacing this process; it returns only on failure, with the exit
 * status. */
int execvm(const char *vm, const char *path, char **args, int nargs, bool dis, const Config *c);

/* report writes translation errors to stderr, with a hint when a file
 * could not be linked and there is no root whose library was searched. */
void report(const StrVec *errs, const Config *c);

/* Paths, as Go's path/filepath (Clean, Join, Abs) and os.Executable. */
char *pathclean(const char *p);
char *pathjoin(const char *dir, const char *name);
char *pathabs(const char *p);
char *executable(const char *argv0);

#endif
