/*
 * raster.h -- gcon's own rasterizer: polygons and strokes into a
 * coverage mask, in plain C. MIT licence, see ../LICENSE.md.
 *
 * Coordinates are device pixels, doubles; pixel (x, y) is the unit
 * square whose centre is (x + 0.5, y + 0.5).
 *
 * Aliased (X's rule): a pixel is covered, 255, when its centre is
 * inside, a centre exactly on an edge counting as inside for a top or
 * left edge and outside for a bottom or right one, as X fills polygons
 * and draws its wide lines and arcs. Antialiased: a pixel's value is the
 * part of it inside, 0 to 255 (16 sub-scanlines a pixel, each covered
 * exactly across). Either way a pixel never decreases, so pieces drawn
 * one after another make their union.
 */
#ifndef GCON_RASTER_H
#define GCON_RASTER_H
#include <stdint.h>

#include <math.h>
#ifndef M_PI	/* math.h's, but not in strict C (glibc) */
#define M_PI 3.14159265358979323846
#endif

typedef struct {
   double x, y;
} RPt;

/* A path: subpaths of points, each closed when filled. */
typedef struct {
   RPt *p;
   int n, cap;
   int *start;			/* where each subpath begins */
   int ns, scap;
} RPath;

void rp_init(RPath *p);
void rp_free(RPath *p);
void rp_clear(RPath *p);
void rp_move(RPath *p, double x, double y);
void rp_line(RPath *p, double x, double y);

/* The mask: stride bytes a row; drawing is limited to the clip
 * rectangle r = x0, y0, x1, y1 (device pixels, half-open). */
typedef struct {
   uint8_t *m;
   int stride;
   int r[4];
} RMask;

/* Fill the path's subpaths (each closed), even-odd or nonzero, aliased
 * or antialiased. */
void r_fill(const RMask *k, const RPath *p, int evenodd, int aa);

/* Stroke the polyline of n points (closed: its last point joins its
 * first) with a line of width w: segments with mitred joins (bevelled
 * where the angle is under 11 degrees, as X's), and at its ends, unless
 * closed, square caps (projecting w/2) or butt ones; one shape, filled
 * nonzero. A polyline of one point, or of points all the same, is a w by
 * w square (square caps) or nothing. */
void r_stroke(const RMask *k, const RPt *pts, int n, int closed, double w, int squarecaps, int aa);

#endif
