/*
 * variants: damaged copies of corpus files, for the differential tests
 * (diff.sh), made with a fixed seed so that every run tests the same.
 *
 *   variants DIR < LIST
 *
 * For every 7th file named in LIST (one per line): three truncations
 * (DIR/tK-1.icn ...) and one copy with ten bytes changed (DIR/mK.icn);
 * then 50 files of up to 2,000 random bytes (DIR/rN.icn), which reach
 * the error paths.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state = 1;

static uint64_t rnd(void) { /* splitmix64 */
	uint64_t z = (state += 0x9e3779b97f4a7c15u);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return z ^ (z >> 31);
}

static void put(const char *dir, const char *name, const char *s, size_t n) {
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fwrite(s, 1, n, f);
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: variants DIR < LIST\n");
		return 2;
	}
	const char *dir = argv[1];
	char line[4096], name[64];
	long k = 0;
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\n")] = 0;
		if (k++ % 7)
			continue;
		FILE *f = fopen(line, "rb");
		if (!f)
			continue;
		char *s = NULL;
		size_t n = 0, cap = 0, got;
		do {
			if (n == cap)
				s = realloc(s, cap = cap ? 2 * cap : 65536);
			got = fread(s + n, 1, cap - n, f);
			n += got;
		} while (got > 0);
		fclose(f);
		if (n == 0) {
			free(s);
			continue;
		}
		for (int c = 1; c <= 3; c++) {
			snprintf(name, sizeof name, "t%ld-%d.icn", k, c);
			put(dir, name, s, (size_t)(rnd() % n));
		}
		for (int i = 0; i < 10; i++)
			s[rnd() % n] = (char)(rnd() % 256);
		snprintf(name, sizeof name, "m%ld.icn", k);
		put(dir, name, s, n);
		free(s);
	}
	char buf[2000];
	for (int r = 1; r <= 50; r++) {
		size_t n = (size_t)(rnd() % 2000);
		for (size_t i = 0; i < n; i++)
			buf[i] = (char)(rnd() % 256);
		snprintf(name, sizeof name, "r%d.icn", r);
		put(dir, name, buf, n);
	}
	return 0;
}
