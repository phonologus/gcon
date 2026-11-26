/*
 * Images for the graphics: palettes (palette.c, from Icon's rcolor.r)
 * and GIF files (gif.c, from Icon's rimage.r), which are the client's;
 * the display server draws and reads pixels, and reads and writes the
 * other formats.
 */
#ifndef GCON_GIMG_H
#define GCON_GIMG_H

#include <stddef.h>
#include <stdint.h>

#define TCH1 '~'   /* usual transparent character */
#define TCH2 0377  /* alternate transparent character */
#define PCH1 ' '   /* punctuation character */
#define PCH2 ','   /* punctuation character */

typedef struct LinearColor {
	long red, green, blue; /* 0 to 65535, linear */
} LinearColor;

struct palentry {      /* one palette member */
	LinearColor clr; /* its colour */
	char used;       /* nonzero if the character is used */
	char valid;      /* nonzero if the entry is valid and opaque */
	char transpt;    /* nonzero if the character is transparent */
};

/* palnum: the palette a name names: 1 to 6 for "c1" to "c6" (and for
 * NULL, the default), -2 to -256 for "g2" to "g256", 0 for no palette. */
int palnum(const char *s);

/* palsetup: palette p's entries, by character (static; NULL if out of
 * memory). */
struct palentry *palsetup(int p);

/* rgbkey: the key of the closest colour in palette p to r, g, b
 * (each 0.0 to 1.0, linear). */
char *rgbkey(int p, double r, double g, double b);

/* palchars: the characters of palette p, and their number. */
const char *palchars(int p, int *n);

/* The characters, in order, for one-character strings. */
extern const unsigned char allchars[256];

/* A GIF image: its size, palette (256 entries) and pixels, one byte
 * each, as palette indexes. */
struct imgdata {
	int width, height;
	struct palentry *paltbl;
	unsigned char *data;
};

/* gif_read decodes a GIF file's bytes; p, the palette for a
 * transparent colour index (0: as the file says). 1, or 0 if it is not
 * a GIF file Icon reads. */
int gif_read(const unsigned char *b, size_t n, int p, struct imgdata *imd);

/* gif_write encodes width x height pixels, each 15-bit linear colour
 * (5 bits each of red, green and blue), as a GIF file's bytes (in *out,
 * malloc'd, and *outlen); 0 if out of memory. */
int gif_write(const short *data, int width, int height, unsigned char **out, size_t *outlen);

#endif
