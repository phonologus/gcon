/*
 * gcon runs Icon programs (the Go reference's cmd/gcon).
 *
 *	gcon run file [args...]        run Icon source, or a built program
 *	gcon build [-o out] file.icn   build a program: an executable file
 *	gcon dis file                  list the VM code of source or a built program
 *	gcon fmt [-l] [-w] [-d] [path ...]
 *	                               format Icon source
 *	gcon env                       print gcon's root and library search paths
 *	gcon version                   print gcon's version
 *
 * Programs run on gcon-vm, the root's (libexec/gcon-vm) or $GCONVM's.
 */
#include "driver.h"
#include "format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void usage(void) {
	fputs("usage:\n"
	      "\tgcon run file [args...]\n"
	      "\tgcon build [-o out] file.icn\n"
	      "\tgcon dis file\n"
	      "\tgcon fmt [-l] [-w] [-d] [path ...]\n"
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
		if (!strcmp(from, "GCONVM"))
			fprintf(stderr, "gcon: GCONVM=go: this gcon has no Go VM; unset GCONVM to use gcon-vm\n");
		else
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
