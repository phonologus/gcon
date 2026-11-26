/*
 * compare -- compare two GIF images pixel by pixel.
 *
 *   compare EXPECTED.gif ACTUAL.gif [DIFF.png]
 *
 * Prints the number of differing pixels. DIFF.png (optional) shows the
 * expected image in grey, with pixels only the actual image has in red
 * and pixels only the expected image has in blue (orange where neither
 * is the background). Exit status: 0 if identical, 1 if they differ, 2
 * on error.
 *
 * Icon writes GIF87a with one image and a global colour table; this
 * reader handles that, plus local colour tables and interlacing for
 * safety. The PNG is written with stored (uncompressed) deflate blocks,
 * so that nothing but libc is needed. Built by tests/ref.sh.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char r, g, b; } RGB;

static const char *curfile;

static void fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "compare: %s: ", curfile);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(2);
}

static unsigned char *readfile(const char *name, size_t *n) {
	FILE *f = fopen(name, "rb");
	if (!f)
		fail("cannot open");
	unsigned char *d = NULL;
	size_t cap = 0, got;
	*n = 0;
	do {
		if (*n == cap && !(d = realloc(d, cap = cap ? 2 * cap : 65536)))
			fail("out of memory");
		got = fread(d + *n, 1, cap - *n, f);
		*n += got;
	} while (got > 0);
	fclose(f);
	return d;
}

/* GIF LZW: the variable-width codes in sub (n bytes) to npix indexes. */
static unsigned char *lzw(const unsigned char *sub, size_t n, int minsize, size_t npix) {
	enum { MAXCODES = 4096 };
	static uint16_t prefix[MAXCODES];
	static unsigned char suffix[MAXCODES], first[MAXCODES];
	static uint32_t len[MAXCODES];
	static unsigned char stack[MAXCODES + 1];
	int clear = 1 << minsize, end = clear + 1, size = minsize + 1, next = clear + 2;
	int prev = -1;
	for (int i = 0; i < clear; i++) {
		suffix[i] = first[i] = (unsigned char)i;
		len[i] = 1;
	}
	unsigned char *out = malloc(npix ? npix : 1);
	size_t nout = 0, bitpos = 0, nbits = n * 8;
	while (bitpos + (size_t)size <= nbits && nout < npix) {
		int code = 0;
		for (int i = 0; i < size; i++)
			if (sub[(bitpos + i) >> 3] >> ((bitpos + i) & 7) & 1)
				code |= 1 << i;
		bitpos += (size_t)size;
		if (code == clear) {
			size = minsize + 1;
			next = clear + 2;
			prev = -1;
			continue;
		}
		if (code == end)
			break;
		int entry;
		if (code < next) {
			entry = code;
			if (prev >= 0 && next < MAXCODES) {
				prefix[next] = (uint16_t)prev;
				suffix[next] = first[code];
				first[next] = first[prev];
				len[next] = len[prev] + 1;
				next++;
			}
		} else {
			if (prev < 0 || code != next || next >= MAXCODES)
				fail("bad LZW code");
			prefix[next] = (uint16_t)prev;
			suffix[next] = first[prev];
			first[next] = first[prev];
			len[next] = len[prev] + 1;
			entry = next++;
		}
		/* write entry's string */
		int sp = 0;
		for (int c = entry;; c = prefix[c]) {
			stack[sp++] = suffix[c];
			if (len[c] == 1)
				break;
		}
		while (sp > 0 && nout < npix)
			out[nout++] = stack[--sp];
		prev = entry;
		if (next == (1 << size) && size < 12)
			size++;
	}
	if (nout < npix)
		fail("image data ends early");
	return out;
}

/* readgif decodes the first image of a GIF onto a w by h canvas. */
static RGB *readgif(const char *name, int *w, int *h) {
	size_t n;
	curfile = name;
	unsigned char *d = readfile(name, &n);
#define NEED(k) do { if (pos + (k) > n) fail("truncated"); } while (0)
	if (n < 13 || (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6)))
		fail("not a GIF");
	*w = d[6] | d[7] << 8;
	*h = d[8] | d[9] << 8;
	int flags = d[10];
	size_t pos = 13;
	RGB gct[256], lct[256], *ct = NULL;
	int ngct = 0, nct = 0;
	if (flags & 0x80) {
		ngct = 2 << (flags & 7);
		NEED(3 * (size_t)ngct);
		memcpy(gct, d + pos, 3 * (size_t)ngct);
		pos += 3 * (size_t)ngct;
	}
	for (;;) {
		NEED(1);
		if (d[pos] == 0x21) { /* extension: skip it */
			pos += 2;
			for (;;) {
				NEED(1);
				if (!d[pos])
					break;
				pos += d[pos] + 1u;
			}
			pos++;
			continue;
		}
		if (d[pos] != 0x2C)
			fail("no image");
		NEED(10);
		int ix = d[pos + 1] | d[pos + 2] << 8, iy = d[pos + 3] | d[pos + 4] << 8;
		int iw = d[pos + 5] | d[pos + 6] << 8, ih = d[pos + 7] | d[pos + 8] << 8;
		int f = d[pos + 9];
		pos += 10;
		ct = gct;
		nct = ngct;
		if (f & 0x80) {
			nct = 2 << (f & 7);
			NEED(3 * (size_t)nct);
			memcpy(lct, d + pos, 3 * (size_t)nct);
			ct = lct;
			pos += 3 * (size_t)nct;
		}
		NEED(1);
		int minsize = d[pos++];
		if (minsize < 1 || minsize > 11)
			fail("bad LZW code size");
		unsigned char *sub = NULL;
		size_t nsub = 0;
		for (;;) {
			NEED(1);
			size_t k = d[pos];
			if (!k)
				break;
			NEED(k + 1);
			sub = realloc(sub, nsub + k);
			memcpy(sub + nsub, d + pos + 1, k);
			nsub += k;
			pos += k + 1;
		}
		size_t npix = (size_t)iw * (size_t)ih;
		unsigned char *idx = lzw(sub, nsub, minsize, npix);
		if (f & 0x40) { /* deinterlace */
			unsigned char *de = malloc(npix ? npix : 1);
			static const int start[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
			size_t row = 0;
			for (int p = 0; p < 4; p++)
				for (int y = start[p]; y < ih; y += step[p])
					memcpy(de + (size_t)y * iw, idx + row++ * iw, (size_t)iw);
			free(idx);
			idx = de;
		}
		RGB *img = calloc((size_t)*w * (size_t)*h + 1, sizeof *img);
		for (int y = 0; y < ih; y++)
			for (int x = 0; x < iw; x++) {
				int c = idx[(size_t)y * iw + x];
				if (c >= nct)
					fail("colour index out of range");
				if (iy + y >= *h || ix + x >= *w)
					fail("image outside the screen");
				img[(size_t)(iy + y) * *w + ix + x] = ct[c];
			}
		free(idx);
		free(sub);
		free(d);
		return img;
	}
#undef NEED
}

/* ------------------------------------------------------------------ */
/* PNG */

static uint32_t crctab[256];

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n) {
	if (!crctab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = v & 1 ? (v >> 1) ^ 0xedb88320u : v >> 1;
			crctab[i] = v;
		}
	c ^= 0xffffffffu;
	for (size_t i = 0; i < n; i++)
		c = crctab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

static void be32(unsigned char *p, uint32_t v) {
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
	unsigned char hdr[8];
	be32(hdr, (uint32_t)n);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	fwrite(data, 1, n, f);
	uint32_t c = crc(crc(0, (const unsigned char *)type, 4), data, n);
	unsigned char t[4];
	be32(t, c);
	fwrite(t, 1, 4, f);
}

static void writepng(const char *name, int w, int h, const RGB *px) {
	size_t rowlen = 1 + 3 * (size_t)w, rawlen = rowlen * (size_t)h;
	unsigned char *raw = malloc(rawlen + 1);
	for (int y = 0; y < h; y++) {
		unsigned char *r = raw + (size_t)y * rowlen;
		r[0] = 0; /* filter: none */
		memcpy(r + 1, px + (size_t)y * w, 3 * (size_t)w);
	}
	/* zlib stream of stored deflate blocks */
	size_t nblocks = rawlen / 65535 + 1;
	unsigned char *z = malloc(2 + rawlen + 5 * nblocks + 4), *q = z;
	*q++ = 0x78;
	*q++ = 0x01;
	size_t left = rawlen, off = 0;
	do {
		size_t k = left < 65535 ? left : 65535;
		*q++ = left == k; /* BFINAL; BTYPE 00 */
		*q++ = (unsigned char)k;
		*q++ = (unsigned char)(k >> 8);
		*q++ = (unsigned char)~k;
		*q++ = (unsigned char)(~k >> 8);
		memcpy(q, raw + off, k);
		q += k;
		off += k;
		left -= k;
	} while (left > 0);
	uint32_t a = 1, b = 0; /* Adler-32 */
	for (size_t i = 0; i < rawlen; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	be32(q, b << 16 | a);
	q += 4;
	FILE *f = fopen(name, "wb");
	if (!f) {
		curfile = name;
		fail("cannot create");
	}
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	unsigned char ihdr[13];
	be32(ihdr, (uint32_t)w);
	be32(ihdr + 4, (uint32_t)h);
	memcpy(ihdr + 8, "\x08\x02\x00\x00\x00", 5); /* 8-bit RGB */
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, (size_t)(q - z));
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
}

static int same(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

int main(int argc, char **argv) {
	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: compare EXPECTED.gif ACTUAL.gif [DIFF.png]\n");
		return 2;
	}
	int ew, eh, aw, ah;
	RGB *e = readgif(argv[1], &ew, &eh);
	RGB *a = readgif(argv[2], &aw, &ah);
	if (ew != aw || eh != ah) {
		printf("size differs: expected %dx%d, got %dx%d\n", ew, eh, aw, ah);
		return 1;
	}
	long diff = 0, total = (long)ew * eh;
	RGB *out = malloc((size_t)total * sizeof *out + 1);
	for (long i = 0; i < total; i++) {
		RGB pe = e[i], pa = a[i], bg = e[0];
		if (same(pe, pa)) {
			int g = 255 - (255 - (pe.r + pe.g + pe.b) / 3) / 3; /* faded expected image */
			out[i] = (RGB){(unsigned char)g, (unsigned char)g, (unsigned char)g};
		} else {
			diff++;
			out[i] = same(pe, bg) ? (RGB){220, 30, 30} : same(pa, bg) ? (RGB){30, 60, 220} : (RGB){230, 140, 0};
		}
	}
	if (argc == 4)
		writepng(argv[3], ew, eh, out);
	printf("%ld of %ld pixels differ (%.2f%%)\n", diff, total, total ? 100.0 * diff / total : 0.0);
	return diff == 0 ? 0 : 1;
}
