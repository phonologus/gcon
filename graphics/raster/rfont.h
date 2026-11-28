/*
 * rfont.h -- fonts for the canvases of raster/ (sdlgfx.h's ft_open,
 * ft_metrics and ft_width, and the drawing rcanvas.c asks of them).
 * MIT licence, see ../LICENSE.md.
 */
#ifndef GCON_RFONT_H
#define GCON_RFONT_H
#include <stdint.h>

/* rf_draw puts the coverage (0 to 255) of n bytes of text, Latin-1, in
 * font f with its baseline at logical (x, y), into the mask (stride
 * bytes a row), at scale s, within the device rectangle r (half-open);
 * a pixel never decreases. */
void rf_draw(void *f, uint8_t *mask, int stride, const int r[4], int s, int x, int y, const char *str, int n);

/* rf_ink gives the horizontal ink extent of the text, logical, from x:
 * at least its advance */
void rf_ink(void *f, const char *str, int n, int *left, int *right);

#endif
