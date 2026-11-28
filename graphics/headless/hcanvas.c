/*
 * hcanvas.c -- the headless display server's canvases (sdlgfx.h's
 * cv_*), drawn by gcon's own rasterizer (../raster). MIT licence, see
 * ../LICENSE.md.
 *
 * As ../src/canvas.c, without Blend2D: pixels are 0xFFRRGGBB at an
 * integer scale; every shape is drawn into an A8 coverage mask (here by
 * raster.c, by X's rules, so aliased) and each covered device pixel is
 * combined with the X GC function, the pattern and the colours. The
 * pixel operations (copy, read, write, erase) are canvas.c's. Text: not
 * yet (the spike's font draws nothing).
 */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdlgfx.h"
#include "raster.h"

typedef struct {
   int w, h;			/* logical size */
   int pw, ph, s;		/* device size, scale */
   uint32_t *px;		/* pw * ph, 0xFFRRGGBB */
   uint8_t *mk;			/* the coverage mask, pw * ph */
} Canvas;

#define OPAQUE 0xFF000000u

static void fill_device(Canvas *c, int x0, int y0, int x1, int y1, uint32_t v)
{
   for (int y = y0; y < y1; y++) {
      uint32_t *p = c->px + (size_t)y * c->pw;
      for (int x = x0; x < x1; x++)
         p[x] = v;
   }
}

/* ---------- the test dump hook, as ../src/canvas.c's ---------- */

typedef struct DumpEnt { void *cv; int id; struct DumpEnt *next; } DumpEnt;
static const char *dumpdir;
static int dumpinit, dumpcount;
static DumpEnt *dumps;

static void mark(const char *name)
{
   char path[4096];
   FILE *f;
   snprintf(path, sizeof path, "%s/%s", dumpdir, name);
   if ((f = fopen(path, "w")) != NULL)
      fclose(f);
}

static void dump_one(void *cv, int id)
{
   Canvas *c = cv;
   char path[4096];
   size_t n = (size_t)c->w * c->h;
   unsigned int *px = malloc(n * sizeof *px + 4);
   unsigned char *rgb = malloc(n * 3 + 1);
   FILE *f;
   if (px && rgb) {
      cv_read(c, 0, 0, c->w, c->h, 0, px);
      for (size_t i = 0; i < n; i++) {
         rgb[3 * i] = px[i] >> 16 & 0xFF;
         rgb[3 * i + 1] = px[i] >> 8 & 0xFF;
         rgb[3 * i + 2] = px[i] & 0xFF;
      }
      snprintf(path, sizeof path, "%s/%03d.ppm", dumpdir, id);
      if ((f = fopen(path, "wb")) != NULL) {
         fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
         fwrite(rgb, 1, n * 3, f);
         fclose(f);
      }
   }
   free(px);
   free(rgb);
}

static void dump_rest(void)
{
   for (DumpEnt *e = dumps; e; e = e->next)
      dump_one(e->cv, e->id);
   dumps = NULL;
   mark("done");
}

static void dump_register(void *cv)
{
   if (!dumpinit) {
      dumpinit = 1;
      dumpdir = getenv("ICONSDL_TEST_DUMP");
      if (dumpdir && !*dumpdir)
         dumpdir = NULL;
      if (dumpdir) {
         atexit(dump_rest);
         signal(SIGTERM, SIG_IGN);
         mark("started");
      }
   }
   if (!dumpdir)
      return;
   DumpEnt *e = malloc(sizeof *e);
   if (!e)
      return;
   e->cv = cv;
   e->id = ++dumpcount;
   e->next = dumps;
   dumps = e;
}

static void dump_unregister(void *cv)
{
   for (DumpEnt **p = &dumps; *p; p = &(*p)->next)
      if ((*p)->cv == cv) {
         DumpEnt *e = *p;
         dump_one(cv, e->id);
         *p = e->next;
         free(e);
         return;
      }
}

/* ---------- life cycle ---------- */

void *cv_new(int w, int h, int scale, unsigned int bg)
{
   Canvas *c = calloc(1, sizeof *c);
   if (!c)
      return NULL;
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   c->w = w; c->h = h; c->s = scale < 1 ? 1 : scale;
   c->pw = w * c->s; c->ph = h * c->s;
   c->px = malloc((size_t)c->pw * c->ph * 4);
   c->mk = calloc((size_t)c->pw, (size_t)c->ph);
   if (!c->px || !c->mk) {
      free(c->px);
      free(c->mk);
      free(c);
      return NULL;
   }
   fill_device(c, 0, 0, c->pw, c->ph, OPAQUE | bg);
   dump_register(c);
   return c;
}

void cv_free(void *cv)
{
   Canvas *c = cv;
   if (!c)
      return;
   dump_unregister(c);
   free(c->px);
   free(c->mk);
   free(c);
}

int cv_scale(void *cv) { return ((Canvas *)cv)->s; }

/* grow (never shrink), keeping the pixels, as X's resizePixmap does */
void cv_resize(void *cv, int w, int h, unsigned int bg)
{
   Canvas *c = cv;
   if (w <= c->w && h <= c->h)
      return;
   if (w < c->w) w = c->w;
   if (h < c->h) h = c->h;
   int pw = w * c->s, ph = h * c->s;
   uint32_t *px = malloc((size_t)pw * ph * 4);
   uint8_t *mk = calloc((size_t)pw, (size_t)ph);
   if (!px || !mk) {
      free(px);
      free(mk);
      return;
   }
   for (int y = 0; y < ph; y++)
      for (int x = 0; x < pw; x++)
         px[(size_t)y * pw + x] = x < c->pw && y < c->ph ? c->px[(size_t)y * c->pw + x] : OPAQUE | bg;
   free(c->px);
   free(c->mk);
   c->px = px;
   c->mk = mk;
   c->w = w; c->h = h; c->pw = pw; c->ph = ph;
}

void cv_dirty(void *cv, int x, int y, int w, int h)
{
   (void)cv; (void)x; (void)y; (void)w; (void)h;
}

/* ---------- combining pixels: the X GC function ---------- */

static inline uint32_t gx(int op, uint32_t s, uint32_t d)
{
   uint32_t r;
   switch (op) {
   case 0:  r = 0; break;
   case 1:  r = s & d; break;
   case 2:  r = s & ~d; break;
   case 3:  r = s; break;
   case 4:  r = ~s & d; break;
   case 5:  r = d; break;
   case 6:  r = s ^ d; break;
   case 7:  r = s | d; break;
   case 8:  r = ~(s | d); break;
   case 9:  r = ~s ^ d; break;
   case 10: r = ~d; break;
   case 11: r = s | ~d; break;
   case 12: r = ~s; break;
   case 13: r = ~s | d; break;
   case 14: r = ~(s & d); break;
   default: r = 0xFFFFFF; break;
   }
   return OPAQUE | (r & 0xFFFFFF);
}

/* the device rectangle an operation may touch: the logical box widened
 * by m, clipped to the clip rectangle and the canvas */
static int region(Canvas *c, const sdlgc *gc, double x0, double y0, double x1, double y1, double m,
   int r[4])
{
   double cx0 = 0, cy0 = 0, cx1 = c->w, cy1 = c->h;
   x0 -= m; y0 -= m; x1 += m; y1 += m;
   if (gc && gc->clipw >= 0) {
      cx0 = gc->clipx; cy0 = gc->clipy;
      cx1 = gc->clipx + gc->clipw; cy1 = gc->clipy + gc->cliph;
   }
   if (x0 < cx0) x0 = cx0;
   if (y0 < cy0) y0 = cy0;
   if (x1 > cx1) x1 = cx1;
   if (y1 > cy1) y1 = cy1;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > c->w) x1 = c->w;
   if (y1 > c->h) y1 = c->h;
   r[0] = (int)floor(x0) * c->s; r[1] = (int)floor(y0) * c->s;
   r[2] = (int)ceil(x1) * c->s;  r[3] = (int)ceil(y1) * c->s;
   return r[0] < r[2] && r[1] < r[3];
}

static void mask_clear(Canvas *c, const int r[4])
{
   for (int y = r[1]; y < r[3]; y++)
      memset(c->mk + (size_t)y * c->pw + r[0], 0, (size_t)(r[2] - r[0]));
}

static RMask maskof(Canvas *c, const int r[4])
{
   RMask k;
   k.m = c->mk;
   k.stride = c->pw;
   memcpy(k.r, r, sizeof k.r);
   return k;
}

/* the mask over r onto the canvas: which 0, the fg (with the pattern);
 * 1, the bg (a striped line's off dashes) */
static void apply(Canvas *c, const sdlgc *gc, const int r[4], int which)
{
   uint32_t fg = gc->fg & 0xFFFFFF, bg = gc->bg & 0xFFFFFF;
   int s = c->s, op = gc->reverse ? 6 : gc->drawop;
   int stip = gc->fillstyle != 0 && gc->pat && gc->patw > 0 && gc->path > 0;
   if (gc->reverse)
      fg ^= bg;
   for (int y = r[1]; y < r[3]; y++) {
      uint32_t *p = c->px + (size_t)y * c->pw;
      const uint8_t *m = c->mk + (size_t)y * c->pw;
      int ly = y / s;
      for (int x = r[0]; x < r[2]; x++) {
         uint32_t src;
         if (m[x] < 128)
            continue;
         if (which == 1)
            src = bg;
         else if (stip) {
            int lx = x / s;
            if (gc->pat[(ly % gc->path) * gc->patw + lx % gc->patw])
               src = fg;
            else if (gc->fillstyle == 3)
               src = bg;
            else
               continue;
         }
         else
            src = fg;
         p[x] = gx(op, src, p[x]);
      }
   }
}

/* ---------- geometry ---------- */

static double width(const sdlgc *gc) { return gc->linewidth > 0 ? gc->linewidth : 1; }

/* fill a path given in logical units */
static void fillpath(Canvas *c, const sdlgc *gc, const RPath *p, int evenodd)
{
   double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
   int r[4];
   RPath d;
   if (p->n < 3)
      return;
   for (int i = 0; i < p->n; i++) {
      x0 = fmin(x0, p->p[i].x); y0 = fmin(y0, p->p[i].y);
      x1 = fmax(x1, p->p[i].x); y1 = fmax(y1, p->p[i].y);
   }
   if (!region(c, gc, x0, y0, x1, y1, 1, r))
      return;
   rp_init(&d);
   for (int s = 0; s < p->ns; s++) {
      int a = p->start[s], b = s + 1 < p->ns ? p->start[s + 1] : p->n;
      for (int i = a; i < b; i++)
         (i == a ? rp_move : rp_line)(&d, p->p[i].x * c->s, p->p[i].y * c->s);
   }
   mask_clear(c, r);
   RMask k = maskof(c, r);
   r_fill(&k, &d, evenodd);
   rp_free(&d);
   apply(c, gc, r, 0);
}

/* stroke the polyline p (logical units, pixel centres) with the line
 * style: solid; dashed, the on dashes (4, or 3 times the width past 1,
 * continuing across vertices) with square caps; striped (X's
 * LineDoubleDash), the whole line in bg, then the on dashes in fg,
 * butt-ended */
static void polyline(Canvas *c, const sdlgc *gc, const RPt *p, int n)
{
   double w = width(gc), x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
   int r[4], closed;
   RPt *d;
   if (n < 2)
      return;
   for (int i = 0; i < n; i++) {
      x0 = fmin(x0, p[i].x); y0 = fmin(y0, p[i].y);
      x1 = fmax(x1, p[i].x); y1 = fmax(y1, p[i].y);
   }
   if (!region(c, gc, x0, y0, x1, y1, w + 1, r))
      return;
   if (!(d = malloc((size_t)n * sizeof *d)))
      return;
   for (int i = 0; i < n; i++) {
      d[i].x = p[i].x * c->s;
      d[i].y = p[i].y * c->s;
   }
   closed = n > 2 && p[0].x == p[n - 1].x && p[0].y == p[n - 1].y;
   RMask k = maskof(c, r);
   if (gc->linestyle == 0) {
      mask_clear(c, r);
      r_stroke(&k, d, n, closed, w * c->s, 1);
      apply(c, gc, r, 0);
      free(d);
      return;
   }
   if (gc->linestyle == 2) {		/* striped: the whole line, in bg */
      mask_clear(c, r);
      r_stroke(&k, d, n, 0, w * c->s, 1);
      apply(c, gc, r, 1);
   }
   mask_clear(c, r);
   double len = (gc->linewidth > 1 ? 3.0 * gc->linewidth : 4.0) * c->s, phase = 0;
   for (int i = 0; i + 1 < n; i++) {
      double dx = d[i + 1].x - d[i].x, dy = d[i + 1].y - d[i].y, seg = hypot(dx, dy), t = 0;
      while (t < seg) {
         double in = fmod(phase, 2 * len);
         int on = in < len;
         double step = fmin((on ? len : 2 * len) - in, seg - t);
         if (on) {
            RPt q[2] = { { d[i].x + dx * t / seg, d[i].y + dy * t / seg },
                         { d[i].x + dx * (t + step) / seg, d[i].y + dy * (t + step) / seg } };
            r_stroke(&k, q, 2, 0, w * c->s, gc->linestyle != 2);
         }
         t += step;
         phase += step;
      }
   }
   apply(c, gc, r, 0);
   free(d);
}

void cv_points(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      int r[4], x = xy[2 * i], y = xy[2 * i + 1];
      if (!region(c, gc, x, y, x + 1, y + 1, 0, r))
         continue;
      if (r[0] != x * c->s || r[1] != y * c->s)
         continue;			/* clipped away */
      r[2] = r[0] + c->s;
      r[3] = r[1] + c->s;
      for (int yy = r[1]; yy < r[3]; yy++)
         memset(c->mk + (size_t)yy * c->pw + r[0], 255, (size_t)c->s);
      apply(c, gc, r, 0);
   }
}

void cv_lines(void *cv, const sdlgc *gc, const int *xy, int n)
{
   RPt *p;
   if (n == 1) {
      cv_points(cv, gc, xy, 1);
      return;
   }
   if (n < 1 || !(p = malloc((size_t)n * sizeof *p)))
      return;
   for (int i = 0; i < n; i++) {
      p[i].x = xy[2 * i] + .5;
      p[i].y = xy[2 * i + 1] + .5;
   }
   polyline(cv, gc, p, n);
   free(p);
}

/* DrawSegment: each segment separately, as X does */
void cv_segments(void *cv, const sdlgc *gc, const int *sg, int n)
{
   for (int i = 0; i < n; i++)
      cv_lines(cv, gc, sg + 4 * i, 2);
}

void cv_rects(void *cv, const sdlgc *gc, const int *r, int n)
{
   for (int i = 0; i < n; i++) {
      double x = r[4 * i] + .5, y = r[4 * i + 1] + .5, w = r[4 * i + 2], h = r[4 * i + 3];
      RPt p[5] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h }, { x, y } };
      if (w == 0 && h == 0) {
         cv_points(cv, gc, r + 4 * i, 1);
         continue;
      }
      polyline(cv, gc, p, 5);
   }
}

void cv_fillrects(void *cv, const sdlgc *gc, const int *r, int n)
{
   RPath p;
   rp_init(&p);
   for (int i = 0; i < n; i++) {
      double x = r[4 * i], y = r[4 * i + 1], w = r[4 * i + 2], h = r[4 * i + 3];
      if (w <= 0 || h <= 0)
         continue;
      rp_clear(&p);
      rp_move(&p, x, y);
      rp_line(&p, x + w, y);
      rp_line(&p, x + w, y + h);
      rp_line(&p, x, y + h);
      fillpath(cv, gc, &p, 0);
   }
   rp_free(&p);
}

/* arcs as canvas.c draws them: centre (x + w/2 + .5, y + h/2 + .5),
 * radii w/2 and h/2, Icon's angles (radians, clockwise with y down),
 * flattened finely enough for the scale */
void cv_arcs(void *cv, const sdlgc *gc, const int *r, const double *a, int n, int fill)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      double x = r[4 * i], y = r[4 * i + 1], w = r[4 * i + 2], h = r[4 * i + 3];
      double cx = x + w / 2 + .5, cy = y + h / 2 + .5, rx = w / 2, ry = h / 2;
      double start = a[2 * i], sweep = a[2 * i + 1];
      int full = fabs(sweep) >= 2 * M_PI - 1e-9;
      int k = (int)(fabs(sweep) * (rx + ry) / 2 * c->s) + 8;
      if (k > 4096)
         k = 4096;
      RPt *p = malloc((size_t)(k + 2) * sizeof *p);
      if (!p)
         continue;
      for (int j = 0; j <= k; j++) {
         double t = start + sweep * j / k;
         p[j].x = cx + rx * cos(t);
         p[j].y = cy + ry * sin(t);
      }
      if (full)
         p[k] = p[0];			/* exactly: a closed outline, joined */
      if (fill) {
         RPath q;
         rp_init(&q);
         if (!full)
            rp_move(&q, cx, cy);
         for (int j = 0; j <= k; j++)
            (j == 0 && full ? rp_move : rp_line)(&q, p[j].x, p[j].y);
         fillpath(c, gc, &q, 0);
         rp_free(&q);
      }
      else
         polyline(c, gc, p, k + 1);
      free(p);
   }
}

void cv_fillpoly(void *cv, const sdlgc *gc, const int *xy, int n)
{
   RPath p;
   if (n < 3)
      return;
   rp_init(&p);
   for (int i = 0; i < n; i++)
      (i ? rp_line : rp_move)(&p, xy[2 * i] + .5, xy[2 * i + 1] + .5);
   fillpath(cv, gc, &p, 1);
   rp_free(&p);
}

/* EraseArea: fill with bg, copy mode, solid (the clip applies) */
void cv_erase(void *cv, const sdlgc *gc, int x, int y, int w, int h)
{
   Canvas *c = cv;
   int r[4];
   if (w <= 0 || h <= 0 || !region(c, gc, x, y, x + w, y + h, 0, r))
      return;
   fill_device(c, r[0], r[1], r[2], r[3], OPAQUE | (gc->bg & 0xFFFFFF));
}

/* CopyArea, as canvas.c's */
void cv_copy(void *srcp, void *dstp, const sdlgc *gc, int x, int y, int w, int h, int x2, int y2)
{
   Canvas *src = srcp, *dst = dstp;
   int r[4], op = gc->reverse ? 6 : gc->drawop, ds = dst->s, ss = src->s;
   uint32_t *tmp;

   if (x < 0) { w += x; x2 -= x; x = 0; }
   if (y < 0) { h += y; y2 -= y; y = 0; }
   if (x + w > src->w) w = src->w - x;
   if (y + h > src->h) h = src->h - y;
   if (w <= 0 || h <= 0 || !region(dst, gc, x2, y2, x2 + w, y2 + h, 0, r))
      return;
   tmp = malloc((size_t)w * ss * h * ss * 4);
   if (!tmp)
      return;
   for (int j = 0; j < h * ss; j++)
      memcpy(tmp + (size_t)j * w * ss, src->px + (size_t)(y * ss + j) * src->pw + (size_t)x * ss,
         (size_t)w * ss * 4);
   for (int dy = r[1]; dy < r[3]; dy++) {
      uint32_t *p = dst->px + (size_t)dy * dst->pw;
      int sy = (dy - y2 * ds) * ss / ds;
      if (sy < 0 || sy >= h * ss)
         continue;
      for (int dx = r[0]; dx < r[2]; dx++) {
         int sx = (dx - x2 * ds) * ss / ds;
         if (sx < 0 || sx >= w * ss)
            continue;
         uint32_t v = tmp[(size_t)sy * w * ss + sx];
         p[dx] = op == 3 ? v : gx(op, v & 0xFFFFFF, p[dx]);
      }
   }
   free(tmp);
}

/* ---------- pixels ---------- */

void cv_read(void *cv, int x, int y, int w, int h, unsigned int bg, unsigned int *out)
{
   Canvas *c = cv;
   for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
         int lx = x + i, ly = y + j;
         if (lx < 0 || ly < 0 || lx >= c->w || ly >= c->h)
            *out++ = bg & 0xFFFFFF;
         else
            *out++ = c->px[(size_t)ly * c->s * c->pw + (size_t)lx * c->s] & 0xFFFFFF;
      }
}

void cv_write(void *cv, const sdlgc *gc, int x, int y, int w, int h, const unsigned int *px,
   const unsigned char *opaque)
{
   Canvas *c = cv;
   int r[4], s = c->s;
   if (!region(c, gc, x, y, x + w, y + h, 0, r))
      return;
   for (int dy = r[1]; dy < r[3]; dy++) {
      uint32_t *p = c->px + (size_t)dy * c->pw;
      int j = dy / s - y;
      if (j < 0 || j >= h)
         continue;
      for (int dx = r[0]; dx < r[2]; dx++) {
         int i = dx / s - x;
         if (i < 0 || i >= w || (opaque && !opaque[j * w + i]))
            continue;
         p[dx] = OPAQUE | (px[j * w + i] & 0xFFFFFF);
      }
   }
}

/* ---------- text: not yet ---------- */

int cv_text(void *cv, const sdlgc *gc, void *font, int x, int y, const char *s, int n, int image)
{
   int asc, desc, maxw, w = ft_width(font, s, n);
   if (image) {
      ft_metrics(font, &asc, &desc, &maxw);
      cv_erase(cv, gc, x, y - asc, w, asc + desc);
   }
   return w;
}
