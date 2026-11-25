/*
 * gcon-vm: the virtual machine for gcon's bytecode (docs/design.md).
 * So far it loads a program and lists it; nothing runs yet.
 */
#include "gcon-vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
	fputs("usage: gcon-vm -dis program\n"
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
	usage();
}
