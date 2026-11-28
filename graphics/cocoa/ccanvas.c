/*
 * ccanvas.c -- the Cocoa display server's canvases (sdlgfx.h's cv_*):
 * Core Graphics bitmap contexts. MIT licence, see ../LICENSE.md.
 *
 * Pixels are 0xFFRRGGBB at an integer scale s: logical pixel (x, y) is
 * the s-by-s block of device pixels at (s*x, s*y). Drawing in copy mode
 * without a pattern is Core Graphics's, antialiased unless the gc's
 * antialias is off (the Mac's own quality, not X's pixels); everything
 * else is drawn aliased into an A8
 * coverage mask, and each covered device pixel is combined with the X
 * GC function, the pattern and the colours, as ../raster/rcanvas.c does it
 * (whose pixel operations, copy, read, write and erase, these are).
 *
 * Geometry follows X's conventions: integer coordinates are pixel
 * centres, so outlines, lines, arcs and polygons go through (x + 0.5,
 * y + 0.5), and rectangle fills cover [x, x+w) by [y, y+h). Lines have
 * projecting caps and mitred joins, dashes of 4 (3 times the width past
 * 1), and striped lines draw their off dashes in the background.
 */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cocoa.h"

typedef struct {
   int w, h;			/* logical size */
   int pw, ph, s;		/* device size, scale */
   uint32_t *px;		/* pw * ph, 0xFFRRGGBB */
   uint8_t *mk;			/* the coverage mask, pw * ph */
   CGContextRef ctx, mctx;	/* over px and mk, in logical units, y down */
   int changed;
} Canvas;

#define OPAQUE 0xFF000000u

static CGColorSpaceRef rgbspace(void)
{
   static CGColorSpaceRef cs;
   if (!cs)
      cs = CGColorSpaceCreateDeviceRGB();
   return cs;
}

/* logical units, y down, for a context over pw x ph device pixels */
static void units(CGContextRef k, int ph, int s)
{
   CGContextTranslateCTM(k, 0, ph);
   CGContextScaleCTM(k, s, -s);
   CGContextSetTextMatrix(k, CGAffineTransformMakeScale(1, -1));
}

static int contexts(Canvas *c)
{
   c->ctx = CGBitmapContextCreate(c->px, (size_t)c->pw, (size_t)c->ph, 8, (size_t)c->pw * 4, rgbspace(),
      (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
   c->mctx = CGBitmapContextCreate(c->mk, (size_t)c->pw, (size_t)c->ph, 8, (size_t)c->pw, NULL,
      (CGBitmapInfo)kCGImageAlphaOnly);
   if (!c->ctx || !c->mctx)
      return 0;
   units(c->ctx, c->ph, c->s);
   units(c->mctx, c->ph, c->s);
   CGContextSetShouldAntialias(c->mctx, false);
   CGContextSetRGBFillColor(c->mctx, 0, 0, 0, 1);
   CGContextSetRGBStrokeColor(c->mctx, 0, 0, 0, 1);
   return 1;
}

static void fill_device(Canvas *c, int x0, int y0, int x1, int y1, uint32_t v)
{
   for (int y = y0; y < y1; y++) {
      uint32_t *p = c->px + (size_t)y * c->pw;
      for (int x = x0; x < x1; x++)
         p[x] = v;
   }
}

/* ---------- the test dump hook, as ../raster/rcanvas.c's ---------- */

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

static void release(Canvas *c)
{
   if (c->ctx)
      CGContextRelease(c->ctx);
   if (c->mctx)
      CGContextRelease(c->mctx);
   free(c->px);
   free(c->mk);
   c->ctx = c->mctx = NULL;
   c->px = NULL;
   c->mk = NULL;
}

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
   if (!c->px || !c->mk || !contexts(c)) {
      release(c);
      free(c);
      return NULL;
   }
   fill_device(c, 0, 0, c->pw, c->ph, OPAQUE | bg);
   c->changed = 1;
   dump_register(c);
   return c;
}

void cv_free(void *cv)
{
   Canvas *c = cv;
   if (!c)
      return;
   dump_unregister(c);
   release(c);
   free(c);
}

int cv_scale(void *cv) { return ((Canvas *)cv)->s; }

/* grow (never shrink), keeping the pixels, as X's resizePixmap does */
void cv_resize(void *cv, int w, int h, unsigned int bg)
{
   Canvas *c = cv, o = *c;
   if (w <= c->w && h <= c->h)
      return;
   if (w < c->w) w = c->w;
   if (h < c->h) h = c->h;
   c->w = w; c->h = h;
   c->pw = w * c->s; c->ph = h * c->s;
   c->px = malloc((size_t)c->pw * c->ph * 4);
   c->mk = calloc((size_t)c->pw, (size_t)c->ph);
   c->ctx = c->mctx = NULL;
   if (!c->px || !c->mk || !contexts(c)) {
      release(c);
      *c = o;			/* keep the old one */
      return;
   }
   fill_device(c, 0, 0, c->pw, c->ph, OPAQUE | bg);
   for (int y = 0; y < o.ph; y++)
      memcpy(c->px + (size_t)y * c->pw, o.px + (size_t)y * o.pw, (size_t)o.pw * 4);
   release(&o);
   c->changed = 1;
}

CGImageRef c_image(void *cv, int *changed)
{
   Canvas *c = cv;
   *changed = c->changed;
   c->changed = 0;
   return CGBitmapContextCreateImage(c->ctx);
}

int c_changed(void *cv) { return ((Canvas *)cv)->changed; }

void cv_dirty(void *cv, int x, int y, int w, int h)
{
   (void)x; (void)y; (void)w; (void)h;
   ((Canvas *)cv)->changed = 1;
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

/*
 * The device rectangle an operation may touch: the logical box [x0, x1)
 * by [y0, y1), widened by m, clipped to the clip rectangle and the
 * canvas.
 */
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
   c->changed = 1;
}

/* ---------- geometry ---------- */

/* copy mode, no pattern: drawn by Core Graphics itself */
static int native(const sdlgc *gc)
{
   return !gc->reverse && gc->drawop == 3 && !(gc->fillstyle != 0 && gc->pat && gc->patw > 0 && gc->path > 0);
}

static void colour(CGContextRef k, unsigned int rgb, int stroke)
{
   double r = (rgb >> 16 & 255) / 255.0, g = (rgb >> 8 & 255) / 255.0, b = (rgb & 255) / 255.0;
   if (stroke)
      CGContextSetRGBStrokeColor(k, r, g, b, 1);
   else
      CGContextSetRGBFillColor(k, r, g, b, 1);
}

static void clipto(CGContextRef k, const sdlgc *gc)
{
   if (gc->clipw >= 0)
      CGContextClipToRect(k, CGRectMake(gc->clipx, gc->clipy, gc->clipw, gc->cliph));
}

static double dashlen(const sdlgc *gc) { return gc->linewidth > 1 ? 3.0 * gc->linewidth : 4.0; }

static void pen(CGContextRef k, const sdlgc *gc)
{
   CGContextSetLineWidth(k, gc->linewidth > 0 ? gc->linewidth : 1);
   CGContextSetLineCap(k, kCGLineCapSquare);
   CGContextSetLineJoin(k, kCGLineJoinMiter);
}

/* dashes: none, the on dashes (phase 0) or the off dashes (phase d) */
static void dashes(CGContextRef k, const sdlgc *gc, int which)
{
   CGFloat d[2];
   if (which < 0) {
      CGContextSetLineDash(k, 0, NULL, 0);
      return;
   }
   d[0] = d[1] = dashlen(gc);
   CGContextSetLineDash(k, which ? d[0] : 0, d, 2);
}

enum { FILL, EOFILL, STROKE };

/* draw path p (fill, even-odd fill, or stroke with the line style) */
static void draw(Canvas *c, const sdlgc *gc, CGPathRef p, int how)
{
   CGRect b = CGPathGetBoundingBox(p);
   double m = how == STROKE ? (gc->linewidth > 1 ? gc->linewidth : 1) + 1.0 : 1.0;
   int r[4];
   if (CGRectIsNull(b) || !region(c, gc, CGRectGetMinX(b), CGRectGetMinY(b), CGRectGetMaxX(b),
          CGRectGetMaxY(b), m, r))
      return;
   if (native(gc)) {
      CGContextRef k = c->ctx;
      CGContextSaveGState(k);
      CGContextSetShouldAntialias(k, gc->antialias != 0);
      clipto(k, gc);
      if (how == STROKE) {
         pen(k, gc);
         if (gc->linestyle == 2) {		/* striped: the bg under the dashes */
            colour(k, gc->bg, 1);
            CGContextAddPath(k, p);
            CGContextStrokePath(k);
         }
         dashes(k, gc, gc->linestyle ? 0 : -1);
         colour(k, gc->fg, 1);
         CGContextAddPath(k, p);
         CGContextStrokePath(k);
      }
      else {
         colour(k, gc->fg, 0);
         CGContextAddPath(k, p);
         if (how == EOFILL)
            CGContextEOFillPath(k);
         else
            CGContextFillPath(k);
      }
      CGContextRestoreGState(k);
      c->changed = 1;
      return;
   }
   for (int pass = 0; pass < (how == STROKE && gc->linestyle == 2 ? 2 : 1); pass++) {
      CGContextRef k = c->mctx;
      mask_clear(c, r);
      CGContextSaveGState(k);
      if (how == STROKE) {
         pen(k, gc);
         dashes(k, gc, gc->linestyle ? pass : -1);
         CGContextAddPath(k, p);
         CGContextStrokePath(k);
      }
      else {
         CGContextAddPath(k, p);
         if (how == EOFILL)
            CGContextEOFillPath(k);
         else
            CGContextFillPath(k);
      }
      CGContextRestoreGState(k);
      apply(c, gc, r, pass);
   }
}

void cv_points(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   CGMutablePathRef p = CGPathCreateMutable();
   for (int i = 0; i < n; i++)
      CGPathAddRect(p, NULL, CGRectMake(xy[2 * i], xy[2 * i + 1], 1, 1));
   if (n > 0)
      draw(c, gc, p, FILL);
   CGPathRelease(p);
}

void cv_lines(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   CGMutablePathRef p;
   if (n < 1)
      return;
   if (n == 1) {
      cv_points(cv, gc, xy, 1);
      return;
   }
   p = CGPathCreateMutable();
   CGPathMoveToPoint(p, NULL, xy[0] + .5, xy[1] + .5);
   for (int i = 1; i < n; i++)
      CGPathAddLineToPoint(p, NULL, xy[2 * i] + .5, xy[2 * i + 1] + .5);
   if (n > 2 && xy[0] == xy[2 * n - 2] && xy[1] == xy[2 * n - 1])
      CGPathCloseSubpath(p);
   draw(c, gc, p, STROKE);
   CGPathRelease(p);
}

void cv_segments(void *cv, const sdlgc *gc, const int *s, int n)
{
   Canvas *c = cv;
   CGMutablePathRef p = CGPathCreateMutable();
   for (int i = 0; i < n; i++) {
      CGPathMoveToPoint(p, NULL, s[4 * i] + .5, s[4 * i + 1] + .5);
      CGPathAddLineToPoint(p, NULL, s[4 * i + 2] + .5, s[4 * i + 3] + .5);
   }
   if (n > 0)
      draw(c, gc, p, STROKE);
   CGPathRelease(p);
}

void cv_rects(void *cv, const sdlgc *gc, const int *r, int n)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      double x = r[4 * i] + .5, y = r[4 * i + 1] + .5, w = r[4 * i + 2], h = r[4 * i + 3];
      CGMutablePathRef p;
      if (w == 0 && h == 0) {
         cv_points(cv, gc, r + 4 * i, 1);
         continue;
      }
      p = CGPathCreateMutable();
      CGPathMoveToPoint(p, NULL, x, y);
      CGPathAddLineToPoint(p, NULL, x + w, y);
      CGPathAddLineToPoint(p, NULL, x + w, y + h);
      CGPathAddLineToPoint(p, NULL, x, y + h);
      CGPathCloseSubpath(p);
      draw(c, gc, p, STROKE);
      CGPathRelease(p);
   }
}

void cv_fillrects(void *cv, const sdlgc *gc, const int *r, int n)
{
   Canvas *c = cv;
   CGMutablePathRef p = CGPathCreateMutable();
   for (int i = 0; i < n; i++)
      if (r[4 * i + 2] > 0 && r[4 * i + 3] > 0)
         CGPathAddRect(p, NULL, CGRectMake(r[4 * i], r[4 * i + 1], r[4 * i + 2], r[4 * i + 3]));
   if (!CGPathIsEmpty(p))
      draw(c, gc, p, FILL);
   CGPathRelease(p);
}

/* Icon's angles: radians from three o'clock, increasing clockwise with
 * y down, as Core Graphics's in these (flipped) units */
void cv_arcs(void *cv, const sdlgc *gc, const int *r, const double *a, int n, int fill)
{
   Canvas *c = cv;
   for (int i = 0; i < n; i++) {
      double x = r[4 * i], y = r[4 * i + 1], w = r[4 * i + 2], h = r[4 * i + 3];
      double cx = x + w / 2 + .5, cy = y + h / 2 + .5, rx = w / 2, ry = h / 2;
      double start = a[2 * i], sweep = a[2 * i + 1];
      CGMutablePathRef p = CGPathCreateMutable();
      int full = fabs(sweep) >= 2 * M_PI - 1e-9;
      if (rx > 0 && ry > 0) {
         CGAffineTransform t = CGAffineTransformScale(CGAffineTransformMakeTranslation(cx, cy), rx, ry);
         if (fill && full)
            CGPathAddEllipseInRect(p, NULL, CGRectMake(cx - rx, cy - ry, 2 * rx, 2 * ry));
         else {
            if (fill)
               CGPathMoveToPoint(p, NULL, cx, cy);
            else
               CGPathMoveToPoint(p, &t, cos(start), sin(start));
            CGPathAddRelativeArc(p, &t, 0, 0, 1, start, sweep);
            if (fill)
               CGPathCloseSubpath(p);
         }
      }
      else if (!fill) {			/* a line: the arc of a flat ellipse */
         CGPathMoveToPoint(p, NULL, cx + rx * cos(start), cy + ry * sin(start));
         for (int j = 1; j <= 16; j++) {
            double t = start + sweep * j / 16;
            CGPathAddLineToPoint(p, NULL, cx + rx * cos(t), cy + ry * sin(t));
         }
      }
      if (!CGPathIsEmpty(p))
         draw(c, gc, p, fill ? FILL : STROKE);
      CGPathRelease(p);
   }
}

void cv_fillpoly(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   CGMutablePathRef p;
   if (n < 3)
      return;
   p = CGPathCreateMutable();
   CGPathMoveToPoint(p, NULL, xy[0] + .5, xy[1] + .5);
   for (int i = 1; i < n; i++)
      CGPathAddLineToPoint(p, NULL, xy[2 * i] + .5, xy[2 * i + 1] + .5);
   CGPathCloseSubpath(p);
   draw(c, gc, p, EOFILL);
   CGPathRelease(p);
}

/* EraseArea: fill with bg, copy mode, solid (the clip applies) */
void cv_erase(void *cv, const sdlgc *gc, int x, int y, int w, int h)
{
   Canvas *c = cv;
   int r[4];
   if (w <= 0 || h <= 0 || !region(c, gc, x, y, x + w, y + h, 0, r))
      return;
   fill_device(c, r[0], r[1], r[2], r[3], OPAQUE | (gc->bg & 0xFFFFFF));
   c->changed = 1;
}

/* CopyArea, as ../raster/rcanvas.c's */
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
   /* a copy of the source region first (they may overlap) */
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
   dst->changed = 1;
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
   c->changed = 1;
}

/* ---------- text ---------- */

static CTLineRef textline(void *font, const char *s, int n)
{
   CFStringRef str = CFStringCreateWithBytes(NULL, (const UInt8 *)s, n, kCFStringEncodingISOLatin1, false);
   const void *keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
   const void *vals[] = { c_font(font), kCFBooleanTrue };
   CFDictionaryRef attrs;
   CFAttributedStringRef as;
   CTLineRef line;
   if (!str)
      return NULL;
   attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
   as = CFAttributedStringCreate(NULL, str, attrs);
   line = CTLineCreateWithAttributedString(as);
   CFRelease(as);
   CFRelease(attrs);
   CFRelease(str);
   return line;
}

int ft_width(void *f, const char *s, int n)
{
   CTLineRef line;
   int w;
   if (!f || n <= 0 || !(line = textline(f, s, n)))
      return 0;
   w = (int)lround(CTLineGetTypographicBounds(line, NULL, NULL, NULL));
   CFRelease(line);
   return w;
}

/* DrawString (image 0), or write()'s text (image 1: the text's cell is
 * first filled with bg, in copy mode, as X's XDrawImageString does);
 * returns the width */
int cv_text(void *cv, const sdlgc *gc, void *font, int x, int y, const char *s, int n, int image)
{
   Canvas *c = cv;
   CTLineRef line;
   int w, asc, desc, maxw, r[4];
   CGRect ink;
   if (!font || n <= 0 || !(line = textline(font, s, n)))
      return 0;
   w = (int)lround(CTLineGetTypographicBounds(line, NULL, NULL, NULL));
   ft_metrics(font, &asc, &desc, &maxw);
   if (image)
      cv_erase(c, gc, x, y - asc, w, asc + desc);
   ink = CTLineGetImageBounds(line, c->ctx);
   if (!region(c, gc, x + fmin(0, CGRectGetMinX(ink)) - 1, y - asc - 1,
          x + fmax(w, CGRectGetMaxX(ink)) + 1, y + desc + 1, 0, r)) {
      CFRelease(line);
      return w;
   }
   if (native(gc)) {
      CGContextSaveGState(c->ctx);
      CGContextSetShouldAntialias(c->ctx, gc->antialias != 0);
      clipto(c->ctx, gc);
      colour(c->ctx, gc->fg, 0);
      CGContextSetTextPosition(c->ctx, x, y);
      CTLineDraw(line, c->ctx);
      CGContextRestoreGState(c->ctx);
      c->changed = 1;
   }
   else {
      mask_clear(c, r);
      CGContextSetTextPosition(c->mctx, x, y);
      CTLineDraw(line, c->mctx);
      apply(c, gc, r, 0);
   }
   CFRelease(line);
   return w;
}
