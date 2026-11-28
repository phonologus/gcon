/*
 * raster.c -- gcon's own rasterizer (raster.h). MIT licence, see
 * ../LICENSE.md.
 *
 * Filling is by scanlines through pixel centres: on the row whose
 * centres are at y + 0.5, an edge from y0 to y1 (y0 < y1) crosses when
 * y0 <= y + 0.5 < y1, and a span from xl to xr covers the pixels whose
 * centres satisfy xl <= x + 0.5 < xr: so a centre on a top or left edge
 * is in, one on a bottom or right edge out, as X has it. A stroke is
 * the union of convex pieces, each filled alone: a rectangle per
 * segment, a wedge per join (a mitre, or a bevel past X's limit), and
 * squares projecting at the ends for square caps.
 */
#include "raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void rp_init(RPath *p) { memset(p, 0, sizeof *p); }

void rp_free(RPath *p)
{
   free(p->p);
   free(p->start);
   rp_init(p);
}

void rp_clear(RPath *p) { p->n = p->ns = 0; }

static int grow(void **v, int *cap, int need, size_t size)
{
   if (need <= *cap)
      return 1;
   int c = *cap ? *cap * 2 : 16;
   while (c < need)
      c *= 2;
   void *n = realloc(*v, (size_t)c * size);
   if (!n)
      return 0;
   *v = n;
   *cap = c;
   return 1;
}

void rp_move(RPath *p, double x, double y)
{
   if (!grow((void **)&p->start, &p->scap, p->ns + 1, sizeof *p->start))
      return;
   p->start[p->ns++] = p->n;
   rp_line(p, x, y);
}

void rp_line(RPath *p, double x, double y)
{
   if (p->ns == 0) {
      rp_move(p, x, y);
      return;
   }
   if (!grow((void **)&p->p, &p->cap, p->n + 1, sizeof *p->p))
      return;
   p->p[p->n].x = x;
   p->p[p->n].y = y;
   p->n++;
}

/* ---------- filling ---------- */

typedef struct {
   double y0, y1;		/* y0 < y1 */
   double x0, dxdy;		/* x at y0, and its change with y */
   int dir;			/* +1 downwards, -1 upwards */
} Edge;

typedef struct {
   double x;
   int dir;
} Cross;

static int cmpcross(const void *a, const void *b)
{
   double d = ((const Cross *)a)->x - ((const Cross *)b)->x;
   return d < 0 ? -1 : d > 0;
}

static void span(const RMask *k, int y, double xl, double xr)
{
   int a = (int)ceil(xl - 0.5), b = (int)ceil(xr - 0.5);
   if (a < k->r[0]) a = k->r[0];
   if (b > k->r[2]) b = k->r[2];
   if (a < b)
      memset(k->m + (size_t)y * k->stride + a, 255, (size_t)(b - a));
}

void r_fill(const RMask *k, const RPath *p, int evenodd)
{
   Edge *e;
   Cross *c;
   int ne = 0;
   double ymin = 1e300, ymax = -1e300;

   if (p->n < 3)
      return;
   e = malloc((size_t)p->n * sizeof *e);
   c = malloc((size_t)p->n * sizeof *c);
   if (!e || !c) {
      free(e);
      free(c);
      return;
   }
   for (int s = 0; s < p->ns; s++) {
      int a = p->start[s], b = s + 1 < p->ns ? p->start[s + 1] : p->n;
      for (int i = a; i < b; i++) {
         RPt u = p->p[i], v = p->p[i + 1 < b ? i + 1 : a];
         if (u.y == v.y)
            continue;
         Edge *x = &e[ne++];
         x->dir = u.y < v.y ? 1 : -1;
         if (u.y > v.y) {
            RPt t = u;
            u = v;
            v = t;
         }
         x->y0 = u.y;
         x->y1 = v.y;
         x->x0 = u.x;
         x->dxdy = (v.x - u.x) / (v.y - u.y);
         if (u.y < ymin) ymin = u.y;
         if (v.y > ymax) ymax = v.y;
      }
   }
   int y0 = (int)floor(ymin), y1 = (int)ceil(ymax);
   if (y0 < k->r[1]) y0 = k->r[1];
   if (y1 > k->r[3]) y1 = k->r[3];
   for (int y = y0; y < y1; y++) {
      double yc = y + 0.5;
      int nc = 0;
      for (int i = 0; i < ne; i++)
         if (e[i].y0 <= yc && yc < e[i].y1) {
            c[nc].x = e[i].x0 + (yc - e[i].y0) * e[i].dxdy;
            c[nc].dir = e[i].dir;
            nc++;
         }
      if (nc < 2)
         continue;
      qsort(c, (size_t)nc, sizeof *c, cmpcross);
      if (evenodd)
         for (int i = 0; i + 1 < nc; i += 2)
            span(k, y, c[i].x, c[i + 1].x);
      else {
         int wind = 0;
         for (int i = 0; i + 1 < nc; i++) {
            wind += c[i].dir;
            if (wind != 0)
               span(k, y, c[i].x, c[i + 1].x);
         }
      }
   }
   free(e);
   free(c);
}

/* ---------- stroking ---------- */

static void poly(const RMask *k, const RPt *q, int n)
{
   RPath p;
   rp_init(&p);
   for (int i = 0; i < n; i++)
      (i ? rp_line : rp_move)(&p, q[i].x, q[i].y);
   r_fill(k, &p, 1);
   rp_free(&p);
}

static RPt pt(double x, double y)
{
   RPt p;
   p.x = x;
   p.y = y;
   return p;
}

/* the join at p between a segment arriving along u1 and one leaving
 * along u2 (unit vectors), for half-width h: on the outer side, a mitre
 * or, where the lines meet at under 11 degrees, a bevel */
static void join(const RMask *k, RPt p, RPt u1, RPt u2, double h)
{
   double dot = u1.x * u2.x + u1.y * u2.y, cross = u1.x * u2.y - u1.y * u2.x;
   if (fabs(cross) < 1e-12 && dot > 0)
      return;				/* straight on */
   RPt n1 = pt(-u1.y * h, u1.x * h), n2 = pt(-u2.y * h, u2.x * h);
   /* the outer side: where the first segment's offset points against
    * the second's direction */
   double sgn = n1.x * u2.x + n1.y * u2.y < 0 ? 1 : -1;
   RPt a = pt(p.x + sgn * n1.x, p.y + sgn * n1.y), b = pt(p.x + sgn * n2.x, p.y + sgn * n2.y);
   double turn = acos(fmax(-1, fmin(1, dot)));	/* 0 straight, pi back */
   double inner = M_PI - turn;			/* the angle the lines make */
   if (inner < 11.0 * M_PI / 180.0) {		/* X: a bevel */
      RPt q[3] = { p, a, b };
      poly(k, q, 3);
      return;
   }
   double mx = a.x - p.x + b.x - p.x, my = a.y - p.y + b.y - p.y, ml = hypot(mx, my);
   double len = h / cos(turn / 2);
   RPt tip = ml > 0 ? pt(p.x + mx / ml * len, p.y + my / ml * len) : a;
   RPt q[4] = { p, a, tip, b };
   poly(k, q, 4);
}

void r_stroke(const RMask *k, const RPt *pts, int n, int closed, double w, int squarecaps)
{
   double h = w / 2;
   int same = 1;
   for (int i = 1; i < n; i++)
      if (pts[i].x != pts[0].x || pts[i].y != pts[0].y)
         same = 0;
   if (n < 1)
      return;
   if (same) {				/* a dot */
      if (squarecaps) {
         RPt q[4] = { pt(pts[0].x - h, pts[0].y - h), pt(pts[0].x + h, pts[0].y - h),
                      pt(pts[0].x + h, pts[0].y + h), pt(pts[0].x - h, pts[0].y + h) };
         poly(k, q, 4);
      }
      return;
   }
   /* the segments, skipping repeated points */
   RPt *v = malloc((size_t)(n + 1) * sizeof *v);
   int m = 0;
   if (!v)
      return;
   for (int i = 0; i < n; i++)
      if (m == 0 || pts[i].x != v[m - 1].x || pts[i].y != v[m - 1].y)
         v[m++] = pts[i];
   if (closed && m > 2 && v[m - 1].x == v[0].x && v[m - 1].y == v[0].y)
      m--;				/* the closing point is the first */
   if (closed && m < 3)
      closed = 0;
   int nseg = closed ? m : m - 1;
   for (int i = 0; i < nseg; i++) {
      RPt a = v[i], b = v[(i + 1) % m];
      double dx = b.x - a.x, dy = b.y - a.y, l = hypot(dx, dy);
      RPt u = pt(dx / l, dy / l), nn = pt(-u.y * h, u.x * h);
      if (!closed && squarecaps) {
         if (i == 0)
            a = pt(a.x - u.x * h, a.y - u.y * h);
         if (i == nseg - 1)
            b = pt(b.x + u.x * h, b.y + u.y * h);
      }
      RPt q[4] = { pt(a.x + nn.x, a.y + nn.y), pt(b.x + nn.x, b.y + nn.y),
                   pt(b.x - nn.x, b.y - nn.y), pt(a.x - nn.x, a.y - nn.y) };
      poly(k, q, 4);
   }
   /* the joins: at each inner point, and all round if closed */
   for (int i = closed ? 0 : 1; i < (closed ? m : m - 1); i++) {
      RPt p = v[i], a = v[(i + m - 1) % m], b = v[(i + 1) % m];
      double l1 = hypot(p.x - a.x, p.y - a.y), l2 = hypot(b.x - p.x, b.y - p.y);
      join(k, p, pt((p.x - a.x) / l1, (p.y - a.y) / l1), pt((b.x - p.x) / l2, (b.y - p.y) / l2), h);
   }
   free(v);
}
