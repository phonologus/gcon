/*
 * bdf.h -- bitmap fonts compiled in from BDF files (bdf2c.sh writes
 * the tables, from deps/fixed). MIT licence, see ../LICENSE.md.
 */
#ifndef GCON_BDF_H
#define GCON_BDF_H

typedef struct {
   short dw;			/* advance */
   short w, h, xo, yo;		/* the bitmap's box: its lower left is (xo, yo) from the origin, y up */
   int bits;			/* where its rows begin in the font's bits ((w + 7) / 8 bytes a row); -1: no glyph */
} BdfGlyph;

typedef struct {
   const char *name;		/* "6x13", "7x13B" */
   int pixel, ascent, descent, maxw;
   int bold, italic;		/* B and O faces */
   const BdfGlyph *g;		/* 256 */
   const unsigned char *bits;
} BdfFont;

#endif
