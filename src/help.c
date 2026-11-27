/*
 * gcon help (help.h), as go help: an overview, a page for each command
 * and some topics. Asked for, help goes to standard output with exit
 * status 0; on misuse, the usage goes to standard error with status 2.
 */
#include "help.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HelpPage {
	const char *name;
	const char *text;
} HelpPage;

#include "helppages.inc"

enum { NPages = sizeof helppages / sizeof helppages[0] };

static const char *find(const char *name) {
	for (int i = 0; i < NPages; i++)
		if (!strcmp(helppages[i].name, name))
			return helppages[i].text;
	return NULL;
}

bool help_isflag(const char *a) {
	if (a[0] != '-')
		return false;
	a += 1 + (a[1] == '-');
	return !strcmp(a, "h") || !strcmp(a, "help");
}

_Noreturn void help_page(const char *name) {
	fputs(find(name), stdout);
	exit(0);
}

_Noreturn void help_usage(const char *name) {
	const char *t = find(name);
	const char *e = strstr(t, "\n\n");
	fwrite(t, 1, e ? (size_t)(e - t) + 1 : strlen(t), stderr);
	fprintf(stderr, "Run 'gcon help %s' for details.\n", name);
	exit(2);
}

_Noreturn void help_overview(void) {
	fputs(find("gcon"), stderr);
	exit(2);
}

int help_main(int argc, char **argv) {
	if (argc == 1) {
		fputs(find("gcon"), stdout);
		return 0;
	}
	if (argc > 2) {
		fputs("usage: gcon help [command | topic]\n"
		      "Too many arguments given.\n",
			stderr);
		return 2;
	}
	const char *t = help_isflag(argv[1]) ? find("help") : strcmp(argv[1], "gcon") ? find(argv[1]) : NULL;
	if (!t) {
		fprintf(stderr, "gcon help %s: unknown help topic. Run 'gcon help'.\n", argv[1]);
		return 2;
	}
	fputs(t, stdout);
	return 0;
}

int help_count(void) { return NPages; }
const char *help_name(int i) { return helppages[i].name; }
const char *help_text(int i) { return helppages[i].text; }
