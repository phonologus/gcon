/*
 * gcon-vm: the virtual machine for gcon's bytecode (docs/design.md).
 * It loads a built program and runs it, or lists it.
 */
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
	fputs("usage: gcon-vm [-progname name] program [argument ...]\n"
	      "       gcon-vm -dis program\n"
	      "       gcon-vm -version\n",
	      stderr);
	exit(2);
}

/* readfile returns the contents of path, or NULL with errno set. */
static uint8_t *readfile(const char *path, size_t *np) {
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	Buf b = {0};
	char chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&b, chunk, n);
	int err = ferror(f) ? errno : 0;
	fclose(f);
	if (err) {
		free(b.b);
		errno = err;
		return NULL;
	}
	*np = b.n;
	return b.b ? (uint8_t *)b.b : xmalloc(1);
}

/* load returns the program in the file path, a built program or an
 * encoded one, or reports why not and exits. */
static Program *load(const char *path) {
	size_t n, len;
	uint8_t *data = readfile(path, &n);
	if (data == NULL) {
		fprintf(stderr, "gcon-vm: %s: %s\n", path, strerror(errno));
		exit(1);
	}
	const uint8_t *enc = encoded(data, n, &len);
	if (enc == NULL) {
		fprintf(stderr, "%s: not a built gcon program\n", path);
		exit(1);
	}
	Program *p;
	Buf *err = decode(enc, len, &p);
	if (err) {
		fprintf(stderr, "%s: ", path);
		fwrite(err->b, 1, err->n, stderr);
		fputc('\n', stderr);
		exit(1);
	}
	return p; /* data stays: the program's strings point into it */
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "-version") == 0) {
		printf("gcon-vm %s\n", GCONVM_VERSION);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "-dis") == 0) {
		Program *p = load(argv[2]);
		Buf b = {0};
		disasm(&b, p);
		fwrite(b.b, 1, b.n, stdout);
		return fflush(stdout) == 0 ? 0 : 1;
	}
	/* Run the program with its arguments. &progname is its name as
	 * given, as iconx's is the icode file's, unless -progname gives
	 * another: gcon run passes a translated program as /dev/fd/N, with
	 * the name the source would have when built. */
	const char *progname = NULL;
	int i = 1;
	if (argc >= 3 && strcmp(argv[1], "-progname") == 0) {
		progname = argv[2];
		i = 3;
	}
	if (i < argc && argv[i][0] != '-') {
		Program *p = load(argv[i]);
		builtins_init();
		link_program(p);
		return run_program(p, progname ? progname : argv[i], argc - i - 1, argv + i + 1);
	}
	usage();
}
