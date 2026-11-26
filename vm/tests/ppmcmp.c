/* ppmcmp A.ppm B.ppm: "N of M pixels differ", or a size mismatch (tests/iconx.sh). */
#include <stdio.h>
#include <stdlib.h>
static unsigned char *load(const char *p, int *w, int *h) {
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	int mx;
	if (fscanf(f, "P6 %d %d %d", w, h, &mx) != 3) { fclose(f); return NULL; }
	fgetc(f);
	size_t n = (size_t)*w * *h * 3;
	unsigned char *b = malloc(n + 1);
	if (fread(b, 1, n, f) != n) { free(b); b = NULL; }
	fclose(f);
	return b;
}
int main(int argc, char **argv) {
	if (argc != 3) return 2;
	int w1, h1, w2, h2;
	unsigned char *a = load(argv[1], &w1, &h1), *b = load(argv[2], &w2, &h2);
	if (!a || !b) { puts("unreadable"); return 2; }
	if (w1 != w2 || h1 != h2) { printf("size %dx%d vs %dx%d\n", w1, h1, w2, h2); return 1; }
	long d = 0;
	for (long i = 0; i < (long)w1 * h1; i++)
		if (a[3*i] != b[3*i] || a[3*i+1] != b[3*i+1] || a[3*i+2] != b[3*i+2]) d++;
	printf("%ld of %ld pixels differ\n", d, (long)w1 * h1);
	return d != 0;
}
