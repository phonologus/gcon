/*
 * imgcodec.c -- image files other than GIF (imgcodec.h): PNG, JPEG and
 * BMP through stb (deps/stb, vendored and pinned in deps/deps.lock;
 * public domain), QOI written here from its specification. These were
 * the display server's (the protocol's ENCODE and DECODE) until gcon
 * v0.5; in gcon-vm every server has them alike.
 */
#include "imgcodec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* stb, compiled here alone, static, for what Icon needs: in memory,
 * PNG, JPEG and BMP (GIF is gif.c's). Its warnings are its own. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

enum { JPEGQUALITY = 95 }; /* above 90, stb keeps the colour at full resolution */

/* ---------- QOI (qoiformat.org, "The Quite OK Image Format", 1.0) ---------- */

enum {
	QOI_INDEX = 0x00, /* 00xxxxxx */
	QOI_DIFF = 0x40,  /* 01xxxxxx */
	QOI_LUMA = 0x80,  /* 10xxxxxx */
	QOI_RUN = 0xc0,   /* 11xxxxxx */
	QOI_RGB = 0xfe,
	QOI_RGBA = 0xff,
	QOI_MASK = 0xc0,
	QOI_HEADER = 14,
	QOI_END = 8, /* 7 zero bytes and a 1 */
};

typedef struct {
	unsigned char r, g, b, a;
} Rgba;

static int qoihash(Rgba c) { return (c.r * 3 + c.g * 5 + c.b * 7 + c.a * 11) % 64; }

static uint32_t be32(const unsigned char *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t *qoi_decode(const unsigned char *d, size_t n, int *w, int *h) {
	Rgba idx[64], px = {0, 0, 0, 255};
	uint32_t width, height, *out;
	size_t p = QOI_HEADER, k, total;
	int run = 0;

	if (n < QOI_HEADER + QOI_END || memcmp(d, "qoif", 4) != 0)
		return NULL;
	width = be32(d + 4);
	height = be32(d + 8);
	if (width == 0 || height == 0 || (d[12] != 3 && d[12] != 4) || d[13] > 1 || width > INT_MAX / 4 ||
	    height > INT_MAX / 4 || (uint64_t)width * height > (uint64_t)400000000)
		return NULL;
	total = (size_t)width * height;
	if (!(out = malloc(total * sizeof *out)))
		return NULL;
	memset(idx, 0, sizeof idx);
	for (k = 0; k < total; k++) {
		if (run > 0)
			run--;
		else if (p < n - QOI_END) {
			int b1 = d[p++];
			if (b1 == QOI_RGB && p + 3 <= n) {
				px.r = d[p];
				px.g = d[p + 1];
				px.b = d[p + 2];
				p += 3;
			} else if (b1 == QOI_RGBA && p + 4 <= n) {
				px.r = d[p];
				px.g = d[p + 1];
				px.b = d[p + 2];
				px.a = d[p + 3];
				p += 4;
			} else if ((b1 & QOI_MASK) == QOI_INDEX)
				px = idx[b1];
			else if ((b1 & QOI_MASK) == QOI_DIFF) {
				px.r += ((b1 >> 4) & 3) - 2;
				px.g += ((b1 >> 2) & 3) - 2;
				px.b += (b1 & 3) - 2;
			} else if ((b1 & QOI_MASK) == QOI_LUMA && p < n) {
				int b2 = d[p++], dg = (b1 & 0x3f) - 32;
				px.r += dg - 8 + ((b2 >> 4) & 0x0f);
				px.g += dg;
				px.b += dg - 8 + (b2 & 0x0f);
			} else if ((b1 & QOI_MASK) == QOI_RUN)
				run = b1 & 0x3f;
			else {
				free(out);
				return NULL;
			}
			idx[qoihash(px)] = px;
		}
		out[k] = (uint32_t)px.a << 24 | (uint32_t)px.r << 16 | (uint32_t)px.g << 8 | px.b;
	}
	*w = (int)width;
	*h = (int)height;
	return out;
}

typedef struct {
	unsigned char *b;
	size_t n, cap;
	int bad;
} Out;

static void put(Out *o, const void *p, size_t n) {
	if (o->bad)
		return;
	if (o->n + n > o->cap) {
		size_t cap = (o->n + n) * 2 + 256;
		unsigned char *b = realloc(o->b, cap);
		if (!b) {
			o->bad = 1;
			return;
		}
		o->b = b;
		o->cap = cap;
	}
	memcpy(o->b + o->n, p, n);
	o->n += n;
}

static void put1(Out *o, int c) {
	unsigned char b = (unsigned char)c;
	put(o, &b, 1);
}

static void put32(Out *o, uint32_t v) {
	unsigned char b[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8),
	                      (unsigned char)v};
	put(o, b, 4);
}

/* 3 channels (Icon's pixels are opaque), sRGB */
static void qoi_encode(Out *o, const uint32_t *px, int w, int h) {
	Rgba idx[64], prev = {0, 0, 0, 255};
	size_t total = (size_t)w * h;
	int run = 0;

	memset(idx, 0, sizeof idx);
	put(o, "qoif", 4);
	put32(o, (uint32_t)w);
	put32(o, (uint32_t)h);
	put1(o, 3);
	put1(o, 0);
	for (size_t k = 0; k < total; k++) {
		Rgba c = {(unsigned char)(px[k] >> 16), (unsigned char)(px[k] >> 8), (unsigned char)px[k], 255};
		if (c.r == prev.r && c.g == prev.g && c.b == prev.b && c.a == prev.a) {
			if (++run == 62 || k == total - 1) {
				put1(o, QOI_RUN | (run - 1));
				run = 0;
			}
			continue;
		}
		if (run > 0) {
			put1(o, QOI_RUN | (run - 1));
			run = 0;
		}
		int i = qoihash(c);
		Rgba e = idx[i];
		if (e.r == c.r && e.g == c.g && e.b == c.b && e.a == c.a)
			put1(o, QOI_INDEX | i);
		else {
			idx[i] = c;
			int dr = (signed char)(c.r - prev.r), dg = (signed char)(c.g - prev.g),
			    db = (signed char)(c.b - prev.b), drg = dr - dg, dbg = db - dg;
			if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1)
				put1(o, QOI_DIFF | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2));
			else if (dg >= -32 && dg <= 31 && drg >= -8 && drg <= 7 && dbg >= -8 && dbg <= 7) {
				put1(o, QOI_LUMA | (dg + 32));
				put1(o, (drg + 8) << 4 | (dbg + 8));
			} else {
				put1(o, QOI_RGB);
				put1(o, c.r);
				put1(o, c.g);
				put1(o, c.b);
			}
		}
		prev = c;
	}
	put(o, "\0\0\0\0\0\0\0\1", QOI_END);
}

/* ---------- the interface ---------- */

uint32_t *img_decode(const unsigned char *data, size_t n, int *w, int *h) {
	unsigned char *rgba;
	uint32_t *out;
	int comp;

	if (n >= 4 && memcmp(data, "qoif", 4) == 0)
		return qoi_decode(data, n, w, h);
	if (n > INT_MAX || !(rgba = stbi_load_from_memory(data, (int)n, w, h, &comp, 4)))
		return NULL;
	size_t k = (size_t)*w * *h;
	if ((out = malloc(k * sizeof *out + 1)) != NULL)
		for (size_t i = 0; i < k; i++) {
			const unsigned char *p = rgba + 4 * i;
			out[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
		}
	stbi_image_free(rgba);
	return out;
}

static void sink(void *ctx, void *data, int size) { put(ctx, data, (size_t)size); }

int img_writes(const char *ext) {
	return !strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") ||
	       !strcasecmp(ext, "bmp") || !strcasecmp(ext, "qoi");
}

int img_encode(const char *ext, const uint32_t *px, int w, int h, unsigned char **out, size_t *len) {
	enum { PNG, JPEG, BMP, QOI } f;
	Out o = {0};
	unsigned char *rgb;
	int ok;

	if (!strcasecmp(ext, "png"))
		f = PNG;
	else if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg"))
		f = JPEG;
	else if (!strcasecmp(ext, "bmp"))
		f = BMP;
	else if (!strcasecmp(ext, "qoi"))
		f = QOI;
	else
		return -1;
	*out = NULL;
	*len = 0;
	if (w <= 0 || h <= 0 || w > INT_MAX / 3 / h)
		return 0;
	if (f == QOI) {
		qoi_encode(&o, px, w, h);
		ok = !o.bad;
	} else {
		size_t k = (size_t)w * h;
		if (!(rgb = malloc(k * 3)))
			return 0;
		for (size_t i = 0; i < k; i++) {
			rgb[3 * i] = (unsigned char)(px[i] >> 16);
			rgb[3 * i + 1] = (unsigned char)(px[i] >> 8);
			rgb[3 * i + 2] = (unsigned char)px[i];
		}
		ok = f == PNG    ? stbi_write_png_to_func(sink, &o, w, h, 3, rgb, w * 3)
		     : f == JPEG ? stbi_write_jpg_to_func(sink, &o, w, h, 3, rgb, JPEGQUALITY)
		                 : stbi_write_bmp_to_func(sink, &o, w, h, 3, rgb);
		ok = ok && !o.bad;
		free(rgb);
	}
	if (!ok) {
		free(o.b);
		return 0;
	}
	*out = o.b;
	*len = o.n;
	return 1;
}

/* stb_image declares its GIF loader static, which (without GIF) is never
 * defined: GCC warns of that by the state at the end of the file, not
 * the push in force at the declaration (clang checks the rest of this
 * file). */
#pragma GCC diagnostic ignored "-Wunused-function"
