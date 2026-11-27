/*
 * gcon runs Icon programs (the Go reference's cmd/gcon).
 *
 *	gcon run file [args...]        run Icon source, or a built program
 *	gcon build [-o out] file.icn   build a program: an executable file
 *	gcon dis file                  list the VM code of source or a built program
 *	gcon fmt [-l] [-w] [-d] [path ...]
 *	                               format Icon source
 *	gcon lsp [-log file] [-docs dir]
 *	                               the language server
 *	gcon vet [-icon95] file.icn ...
 *	                               report likely mistakes (-icon95: and
 *	                               what is not Icon 9.5)
 *	gcon env                       print gcon's root and library search paths
 *	gcon version                   print gcon's version
 *
 * Programs run on gcon-vm, the root's (libexec/gcon-vm) or $GCONVM's.
 */
#include "driver.h"
#include "format.h"
#include "ipp.h"
#include "lsp.h"
#include "vet.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void usage(void) {
	fputs("usage:\n"
	      "\tgcon run file [args...]\n"
	      "\tgcon build [-o out] file.icn\n"
	      "\tgcon dis file\n"
	      "\tgcon fmt [-l] [-w] [-d] [path ...]\n"
	      "\tgcon lsp [-log file] [-docs dir]\n"
	      "\tgcon vet [-icon95] file.icn ...\n"
	      "\tgcon env\n"
	      "\tgcon version\n",
		stderr);
	exit(2);
}

static Root root;

static Config config(void) {
	Config c = {root.dir};
	return c;
}

/* vm returns the gcon-vm to use, or reports why there is none. */
static const char *vm(void) {
	const char *from;
	const char *p = findvm(root.dir, &from);
	if (!p) {
		fprintf(stderr, "gcon: no gcon-vm: none in the root's libexec, and GCONVM is not set; see gcon env\n");
		exit(1);
	}
	return p;
}

/* build writes the built program, by default named as icont names its
 * output: the main file's name without ".icn", in the current
 * directory. Flags are parsed as Go's flag package parses them. */
static int build(int argc, char **argv, const char *argv0) {
	const char *out = NULL;
	int i = 0;
	for (; i < argc; i++) {
		char *a = argv[i];
		if (a[0] != '-' || !a[1])
			break;
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		char *name = a + 1 + (a[1] == '-');
		char *eq = strchr(name, '=');
		size_t nl = eq ? (size_t)(eq - name) : strlen(name);
		if ((nl == 1 && name[0] == 'h') || (nl == 4 && !strncmp(name, "help", 4)))
			usage();
		if (nl != 1 || name[0] != 'o') {
			fprintf(stderr, "flag provided but not defined: -%.*s\n", (int)nl, name);
			usage();
		}
		if (eq)
			out = eq + 1;
		else if (i + 1 < argc)
			out = argv[++i];
		else {
			fprintf(stderr, "flag needs an argument: -o\n");
			usage();
		}
	}
	if (argc - i != 1)
		usage();
	const char *src = argv[i];
	if (!out) {
		const char *base = strrchr(src, '/');
		base = base ? base + 1 : src;
		size_t n = strlen(base);
		if (n < 4 || strcmp(base + n - 4, ".icn")) {
			fprintf(stderr, "gcon build: %s: name the output with -o\n", src);
			return 2;
		}
		out = xstrndup(base, n - 4);
	}
	Config c = config();
	StrVec errs = {0};
	if (!buildfile(src, out, executable(argv0), &c, &errs)) {
		report(&errs, &c);
		return 1;
	}
	return 0;
}

/* frontinfo is vet's resolver for a program the driver translated. */
static SemaInfo *frontinfo(void *ctx, SemaFile *f) {
	Front *fe = ctx;
	for (size_t i = 0; i < fe->n; i++)
		if (fe->files[i].info->file == f)
			return fe->files[i].info;
	return NULL;
}

/* frontlink is vet's link resolver for a program the driver translated:
 * what front_end recorded for the link declaration's token. */
static VetLinkKind frontlink(void *ctx, SemaInfo *from, Token *name) {
	(void)from;
	Front *fe = ctx;
	for (size_t i = 0; i < fe->nlinks; i++)
		if (fe->links[i].tok == name)
			return (VetLinkKind)fe->links[i].kind;
	return VetLinkOther;
}

/* inlib reports whether path is in the root's library. */
static bool inlib(const char *path) {
	if (!root.dir)
		return false;
	char *lib = pathjoin(root.dir, "lib");
	char *ap = pathabs(path);
	size_t n = strlen(lib);
	return !strncmp(ap, lib, n) && ap[n] == '/';
}

/* vet: gcon vet [-icon95] file.icn ...: each file as a program, with
 * the files it links; findings in files outside the root's library are
 * reported as "file:line: message". Exit status 1 if there are any, 2
 * if a program cannot be translated. */
static int vet(int argc, char **argv) {
	VetOptions opts = {0};
	int i = 0;
	for (; i < argc; i++) {
		char *a = argv[i];
		if (a[0] != '-' || !a[1])
			break;
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		char *name = a + 1 + (a[1] == '-');
		if (!strcmp(name, "h") || !strcmp(name, "help"))
			usage();
		if (strcmp(name, "icon95")) {
			fprintf(stderr, "flag provided but not defined: -%s\n", name);
			usage();
		}
		opts.icon95 = true;
	}
	if (i == argc)
		usage();
	Config c = config();
	int status = 0;
	bool found = false;
	for (; i < argc; i++) {
		const char *path = argv[i];
		FILE *fp = fopen(path, "rb");
		if (!fp) {
			fprintf(stderr, "gcon vet: open %s: %s\n", path, strerror(errno));
			status = 2;
			continue;
		}
		Buf src = {0};
		char chunk[65536];
		size_t k;
		while ((k = fread(chunk, 1, sizeof chunk, fp)) > 0)
			bufadd(&src, chunk, k);
		fclose(fp);
		StrVec errs = {0};
		Front *fe = front_end(path, src.b ? src.b : "", src.n, &c, &errs);
		if (!fe) {
			report(&errs, &c);
			status = 2;
			continue;
		}
		for (size_t j = 0; j < fe->n; j++) {
			if (inlib(fe->paths[j]))
				continue;
			VetFindings out = {0};
			opts.link = frontlink, opts.linkctx = fe;
			vet_file(fe->prog, fe->files[j].info, frontinfo, fe, opts, &out);
			for (size_t m = 0; m < out.n; m++) {
				const char *file;
				long line;
				ipp_locate(fe->files[j].map, out.v[m].offset, &file, &line);
				printf("%s:%ld: %s\n", file, line, out.v[m].msg);
				found = true;
			}
		}
	}
	if (status)
		return status;
	return found ? 1 : 0;
}

/* sq quotes s for sh. */
static void sq(const char *s) {
	putchar('\'');
	for (; *s; s++) {
		if (*s == '\'')
			fputs("'\\''", stdout);
		else
			putchar(*s);
	}
	putchar('\'');
}

static void var(const char *name, const char *val) {
	printf("%s=", name);
	sq(val ? val : "");
	putchar('\n');
}

static void joined(const char *name, StrVec v) {
	Buf b = {0};
	bufputc(&b, '.');
	for (size_t i = 0; i < v.n; i++) {
		bufputc(&b, ':');
		bufputs(&b, v.v[i]);
	}
	var(name, b.b);
}

/* env prints gcon's root and search paths, as go env prints Go's, in a
 * form sh can read. */
static void env(void) {
	Config c = config();
	var("GCONROOT", root.dir);
	var("GCONROOTFROM", root.source);
	var("IPATH", getenv("IPATH"));
	var("LPATH", getenv("LPATH"));
	joined("GCONLINKPATH", cfg_libpath(&c));
	joined("GCONINCLUDEPATH", cfg_includepath(&c));
	const char *from;
	const char *p = findvm(root.dir, &from);
	var("GCONVM", p);
	var("GCONVMFROM", from);
	var("GCONVERSION", GCON_VERSION);
}

int main(int argc, char **argv) {
	if (argc < 2)
		usage();
	const char *cmd = argv[1];
	root = root_find();
	Config c = config();
	if (!strcmp(cmd, "run")) {
		if (argc < 3)
			usage();
		return execvm(vm(), argv[2], argv + 3, argc - 3, false, &c);
	}
	if (!strcmp(cmd, "build"))
		return build(argc - 2, argv + 2, argv[0]);
	if (!strcmp(cmd, "dis")) {
		if (argc != 3)
			usage();
		return execvm(vm(), argv[2], NULL, 0, true, &c);
	}
	if (!strcmp(cmd, "lsp"))
		return lsp_main(argc - 1, argv + 1);
	if (!strcmp(cmd, "vet"))
		return vet(argc - 2, argv + 2);
	if (!strcmp(cmd, "fmt"))
		return format_main(argc - 1, argv + 1);
	if (!strcmp(cmd, "env")) {
		env();
		return 0;
	}
	if (!strcmp(cmd, "version")) {
		printf("gcon %s\n", GCON_VERSION);
		return 0;
	}
	usage();
}
