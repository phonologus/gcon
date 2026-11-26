/*
 * damage PROG DIR: the variants of a built program for damage.sh, as
 * files DIR/00000, DIR/00001, ...: the encoded program (after its
 * "[gcon program follows]" line) truncated at each length, keeping its
 * 8-byte magic (else gcon reads source); then each byte changed two ways
 * (xor 0x01, xor 0x80), with the checksum (the CRC-32 of the encoding
 * before the "End " section, in its last 4 bytes, little-endian) made
 * right again when the "End " section is untouched.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32(const unsigned char *p, size_t n) { /* zlib's */
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
	}
	return c ^ 0xffffffffu;
}

static const char *dir;
static int nout;

static void out(const unsigned char *p, size_t n) {
	char path[4096];
	snprintf(path, sizeof path, "%s/%05d", dir, nout++);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(p, 1, n, f) != n || fclose(f)) {
		perror(path);
		exit(1);
	}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: damage PROG DIR\n");
		return 2;
	}
	dir = argv[2];
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	unsigned char *d = NULL;
	size_t n = 0, cap = 0, got;
	do {
		if (n == cap)
			d = realloc(d, cap = cap ? 2 * cap : 65536);
		got = fread(d + n, 1, cap - n, f);
		n += got;
	} while (got > 0);
	fclose(f);
	static const char marker[] = "\n[gcon program follows]\n\x89GCN\r\n\x1a\n";
	size_t ml = sizeof marker - 1, start = 0;
	for (;; start++) {
		if (start + ml > n) {
			fprintf(stderr, "damage: %s: not built\n", argv[1]);
			return 1;
		}
		if (!memcmp(d + start, marker, ml))
			break;
	}
	start += strlen("\n[gcon program follows]\n");
	for (size_t len = start + 8; len < n; len++)
		out(d, len);
	size_t end = n - 9; /* the "End " section: tag, length 4, the CRC */
	unsigned char *v = malloc(n);
	for (size_t i = start + 8; i < n; i++)
		for (int x = 0x01; x <= 0x80; x += 0x7f) {
			memcpy(v, d, n);
			v[i] ^= (unsigned char)x;
			if (i < end) {
				uint32_t c = crc32(v + start, end - start);
				for (int k = 0; k < 4; k++)
					v[n - 4 + k] = (unsigned char)(c >> (8 * k));
			}
			out(v, n);
		}
	return 0;
}
