/*
 * xcanvas.c -- the X11 display server's canvases (sdlgfx.h's cv_*):
 * each a Pixmap, drawn with X's own requests.
 * MIT licence, see ../LICENSE.md.
 *
 * The protocol's graphics context is X's (../src/sdlgfx.h), set up as
 * Icon's X11 backend sets its GC (icon/src/runtime/rxwin.ri): projecting
 * caps, mitred joins, dashes of 4 (3 times the width past 1), even-odd
 * fills, pie-slice arcs, and drawop=reverse as GXxor with fg ^ bg. So
 * the pixels are X's own. Canvases are at scale 1.
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xutil.h>
#include "x11.h"

typedef struct {
   Pixmap pm;
   int w, h;
   int dirty, d[4];		/* x0, y0, x1, y1 */
} Canvas;

static GC dgc;			/* drawing, set from the protocol's gc */
static Pixmap stip;		/* the stipple, for patterns */
static int stipw, stiph;
static unsigned char *stipbits;	/* its cells, to reuse it */

static void touch(Canvas *c, int x0, int y0, int x1, int y1)
{
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > c->w) x1 = c->w;
   if (y1 > c->h) y1 = c->h;
   if (x0 >= x1 || y0 >= y1)
      return;
   if (!c->dirty) {
      c->d[0] = x0; c->d[1] = y0; c->d[2] = x1; c->d[3] = y1;
      c->dirty = 1;
      return;
   }
   if (x0 < c->d[0]) c->d[0] = x0;
   if (y0 < c->d[1]) c->d[1] = y0;
   if (x1 > c->d[2]) c->d[2] = x1;
   if (y1 > c->d[3]) c->d[3] = y1;
}

/* touch the bounds of n points, widened by the line's reach */
static void touchpts(Canvas *c, const sdlgc *gc, const int *xy, int n)
{
   int m = (gc->linewidth > 1 ? gc->linewidth : 1) + 1;
   int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
   for (int i = 0; i < n; i++) {
      int x = xy[2 * i], y = xy[2 * i + 1];
      if (i == 0 || x < x0) x0 = x;
      if (i == 0 || y < y0) y0 = y;
      if (i == 0 || x > x1) x1 = x;
      if (i == 0 || y > y1) y1 = y;
   }
   if (n > 0)
      touch(c, x0 - m, y0 - m, x1 + m + 1, y1 + m + 1);
}

Pixmap x_pixmap(void *cv, int *w, int *h)
{
   Canvas *c = cv;
   *w = c->w;
   *h = c->h;
   return c->pm;
}

int x_take_dirty(void *cv, int r[4])
{
   Canvas *c = cv;
   if (!c->dirty)
      return 0;
   memcpy(r, c->d, sizeof c->d);
   c->dirty = 0;
   return 1;
}

/* ---------- the test dump hook, as ../raster/rcanvas.c's ---------- */

/*
 * If ICONSDL_TEST_DUMP names a directory when the first canvas is made,
 * every canvas is written there as DIR/NNN.ppm when it is freed, and
 * those still alive when the process exits; DIR/started marks the first
 * canvas, DIR/done that all are written. SIGTERM is ignored, so that a
 * test stopping the program leaves the server to write them.
 */
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

static void fill(Pixmap pm, int x, int y, int w, int h, unsigned int rgb)
{
   XGCValues v;
   v.function = GXcopy;
   v.foreground = x_pixel(rgb);
   v.fill_style = FillSolid;
   XChangeGC(xdpy, dgc, GCFunction | GCForeground | GCFillStyle, &v);
   XSetClipMask(xdpy, dgc, None);
   XFillRectangle(xdpy, pm, dgc, x, y, (unsigned)w, (unsigned)h);
}

void *cv_new(int w, int h, int scale, unsigned int bg)
{
   Canvas *c;
   (void)scale;
   if (!x_open())
      return NULL;
   if (!dgc) {
      XGCValues v;
      v.graphics_exposures = False;
      v.cap_style = CapProjecting;
      v.join_style = JoinMiter;
      v.fill_rule = EvenOddRule;
      v.arc_mode = ArcPieSlice;
      dgc = XCreateGC(xdpy, xroot, GCGraphicsExposures | GCCapStyle | GCJoinStyle | GCFillRule | GCArcMode, &v);
   }
   if (!(c = calloc(1, sizeof *c)))
      return NULL;
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   c->w = w;
   c->h = h;
   c->pm = XCreatePixmap(xdpy, xroot, (unsigned)w, (unsigned)h, (unsigned)xdepth);
   fill(c->pm, 0, 0, w, h, bg);
   touch(c, 0, 0, w, h);
   dump_register(c);
   return c;
}

void cv_free(void *cv)
{
   Canvas *c = cv;
   if (!c)
      return;
   dump_unregister(c);
   XFreePixmap(xdpy, c->pm);
   free(c);
}

int cv_scale(void *cv) { (void)cv; return 1; }

/* grow (never shrink), keeping the pixels, as X's resizePixmap does */
void cv_resize(void *cv, int w, int h, unsigned int bg)
{
   Canvas *c = cv;
   Pixmap pm;
   if (w <= c->w && h <= c->h)
      return;
   if (w < c->w) w = c->w;
   if (h < c->h) h = c->h;
   pm = XCreatePixmap(xdpy, xroot, (unsigned)w, (unsigned)h, (unsigned)xdepth);
   fill(pm, 0, 0, w, h, bg);
   XSetFunction(xdpy, dgc, GXcopy);
   XCopyArea(xdpy, c->pm, pm, dgc, 0, 0, (unsigned)c->w, (unsigned)c->h, 0, 0);
   XFreePixmap(xdpy, c->pm);
   c->pm = pm;
   c->w = w;
   c->h = h;
   touch(c, 0, 0, w, h);
}

/* ---------- the graphics context ---------- */

/* a GC for a depth-1 pixmap that puts an XYBitmap as it is: XPutImage
 * draws its 1 bits in the foreground, its 0 bits in the background */
static GC bitgc(Pixmap pm)
{
   XGCValues v;
   v.foreground = 1;
   v.background = 0;
   return XCreateGC(xdpy, pm, GCForeground | GCBackground, &v);
}

/* the stipple for the gc's pattern (reused while it is the same) */
static Pixmap stipple(const sdlgc *gc)
{
   size_t n = (size_t)gc->patw * gc->path;
   if (stip && stipw == gc->patw && stiph == gc->path && !memcmp(stipbits, gc->pat, n))
      return stip;
   if (stip)
      XFreePixmap(xdpy, stip);
   free(stipbits);
   stipbits = malloc(n);
   if (stipbits)
      memcpy(stipbits, gc->pat, n);
   stipw = gc->patw;
   stiph = gc->path;
   stip = XCreatePixmap(xdpy, xroot, (unsigned)stipw, (unsigned)stiph, 1);
   XImage *im = XCreateImage(xdpy, DefaultVisual(xdpy, DefaultScreen(xdpy)), 1, XYBitmap, 0, NULL,
      (unsigned)stipw, (unsigned)stiph, 8, 0);
   im->data = calloc((size_t)im->bytes_per_line, (size_t)stiph);
   for (int y = 0; y < stiph; y++)
      for (int x = 0; x < stipw; x++)
         XPutPixel(im, x, y, gc->pat[y * stipw + x] ? 1 : 0);
   GC g = bitgc(stip);
   XPutImage(xdpy, stip, g, im, 0, 0, 0, 0, (unsigned)stipw, (unsigned)stiph);
   XFreeGC(xdpy, g);
   XDestroyImage(im);
   if (!stipbits) {			/* cannot be reused */
      stipw = stiph = 0;
   }
   return stip;
}

static void clip(const sdlgc *gc)
{
   if (gc->clipw < 0)
      XSetClipMask(xdpy, dgc, None);
   else {
      XRectangle r;
      r.x = (short)gc->clipx;
      r.y = (short)gc->clipy;
      r.width = (unsigned short)(gc->clipw > 0 ? gc->clipw : 0);
      r.height = (unsigned short)(gc->cliph > 0 ? gc->cliph : 0);
      XSetClipRectangles(xdpy, dgc, 0, 0, &r, 1, Unsorted);
   }
}

/* set dgc from the protocol's gc */
static GC use(const sdlgc *gc)
{
   XGCValues v;
   unsigned long mask = GCFunction | GCForeground | GCBackground | GCLineWidth | GCLineStyle
      | GCDashList | GCFillStyle;
   unsigned long fg = x_pixel(gc->fg), bg = x_pixel(gc->bg);
   int d = gc->linewidth > 1 ? 3 * gc->linewidth : 4;

   v.function = gc->reverse ? GXxor : gc->drawop;
   v.foreground = gc->reverse ? fg ^ bg : fg;
   v.background = bg;
   v.line_width = gc->linewidth;
   v.line_style = gc->linestyle == 1 ? LineOnOffDash : gc->linestyle == 2 ? LineDoubleDash : LineSolid;
   v.dashes = (char)(d > 127 ? 127 : d);
   v.fill_style = FillSolid;
   if (gc->fillstyle != 0 && gc->pat && gc->patw > 0 && gc->path > 0) {
      v.fill_style = gc->fillstyle == 3 ? FillOpaqueStippled : FillStippled;
      v.stipple = stipple(gc);
      v.ts_x_origin = v.ts_y_origin = 0;
      mask |= GCStipple | GCTileStipXOrigin | GCTileStipYOrigin;
   }
   XChangeGC(xdpy, dgc, mask, &v);
   clip(gc);
   return dgc;
}

/* ---------- drawing ---------- */

static XPoint *points(const int *xy, int n)
{
   static XPoint *p;
   static int cap;
   if (n > cap) {
      XPoint *q = realloc(p, (size_t)n * sizeof *p);
      if (!q)
         return NULL;
      p = q;
      cap = n;
   }
   for (int i = 0; i < n; i++) {
      p[i].x = (short)xy[2 * i];
      p[i].y = (short)xy[2 * i + 1];
   }
   return p;
}

void cv_points(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   XPoint *p = points(xy, n);
   if (!p || n <= 0)
      return;
   XDrawPoints(xdpy, c->pm, use(gc), p, n, CoordModeOrigin);
   touchpts(c, gc, xy, n);
}

void cv_lines(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   XPoint *p = points(xy, n);
   if (!p || n <= 0)
      return;
   XDrawLines(xdpy, c->pm, use(gc), p, n, CoordModeOrigin);
   touchpts(c, gc, xy, n);
}

void cv_fillpoly(void *cv, const sdlgc *gc, const int *xy, int n)
{
   Canvas *c = cv;
   XPoint *p = points(xy, n);
   if (!p || n < 3)
      return;
   XFillPolygon(xdpy, c->pm, use(gc), p, n, Complex, CoordModeOrigin);
   touchpts(c, gc, xy, n);
}

void cv_segments(void *cv, const sdlgc *gc, const int *s, int n)
{
   Canvas *c = cv;
   XSegment *g = malloc((size_t)(n > 0 ? n : 1) * sizeof *g);
   if (!g)
      return;
   for (int i = 0; i < n; i++) {
      g[i].x1 = (short)s[4 * i]; g[i].y1 = (short)s[4 * i + 1];
      g[i].x2 = (short)s[4 * i + 2]; g[i].y2 = (short)s[4 * i + 3];
   }
   if (n > 0)
      XDrawSegments(xdpy, c->pm, use(gc), g, n);
   free(g);
   touchpts(c, gc, s, 2 * n);
}

static XRectangle *rects(const int *r, int n)
{
   XRectangle *q = malloc((size_t)(n > 0 ? n : 1) * sizeof *q);
   if (!q)
      return NULL;
   for (int i = 0; i < n; i++) {
      q[i].x = (short)r[4 * i];
      q[i].y = (short)r[4 * i + 1];
      q[i].width = (unsigned short)(r[4 * i + 2] > 0 ? r[4 * i + 2] : 0);
      q[i].height = (unsigned short)(r[4 * i + 3] > 0 ? r[4 * i + 3] : 0);
   }
   return q;
}

static void touchrects(Canvas *c, const sdlgc *gc, const int *r, int n, int m)
{
   if (gc && m)
      m = (gc->linewidth > 1 ? gc->linewidth : 1) + 1;
   for (int i = 0; i < n; i++)
      touch(c, r[4 * i] - m, r[4 * i + 1] - m, r[4 * i] + r[4 * i + 2] + m + 1,
         r[4 * i + 1] + r[4 * i + 3] + m + 1);
}

void cv_rects(void *cv, const sdlgc *gc, const int *r, int n)
{
   Canvas *c = cv;
   XRectangle *q = rects(r, n);
   if (!q)
      return;
   if (n > 0)
      XDrawRectangles(xdpy, c->pm, use(gc), q, n);
   free(q);
   touchrects(c, gc, r, n, 1);
}

void cv_fillrects(void *cv, const sdlgc *gc, const int *r, int n)
{
   Canvas *c = cv;
   XRectangle *q = rects(r, n);
   if (!q)
      return;
   if (n > 0)
      XFillRectangles(xdpy, c->pm, use(gc), q, n);
   free(q);
   touchrects(c, gc, r, n, 0);
}

/* Icon's angles are radians, clockwise from three o'clock (y grows
 * down); X's are 64ths of a degree, anticlockwise. */
static int xangle(double a)
{
   return (int)lround(-a * 180.0 / M_PI * 64.0);
}

void cv_arcs(void *cv, const sdlgc *gc, const int *r, const double *a, int n, int fill)
{
   Canvas *c = cv;
   XArc *q = malloc((size_t)(n > 0 ? n : 1) * sizeof *q);
   if (!q)
      return;
   for (int i = 0; i < n; i++) {
      q[i].x = (short)r[4 * i];
      q[i].y = (short)r[4 * i + 1];
      q[i].width = (unsigned short)(r[4 * i + 2] > 0 ? r[4 * i + 2] : 0);
      q[i].height = (unsigned short)(r[4 * i + 3] > 0 ? r[4 * i + 3] : 0);
      q[i].angle1 = (short)xangle(a[2 * i]);
      q[i].angle2 = (short)xangle(a[2 * i + 1]);
   }
   if (n > 0) {
      if (fill)
         XFillArcs(xdpy, c->pm, use(gc), q, n);
      else
         XDrawArcs(xdpy, c->pm, use(gc), q, n);
   }
   free(q);
   touchrects(c, gc, r, n, 1);
}

/* EraseArea: the bg, in copy mode, solid; the clip applies */
void cv_erase(void *cv, const sdlgc *gc, int x, int y, int w, int h)
{
   Canvas *c = cv;
   XGCValues v;
   if (w <= 0 || h <= 0)
      return;
   v.function = GXcopy;
   v.foreground = x_pixel(gc->bg);
   v.fill_style = FillSolid;
   XChangeGC(xdpy, dgc, GCFunction | GCForeground | GCFillStyle, &v);
   clip(gc);
   XFillRectangle(xdpy, c->pm, dgc, x, y, (unsigned)w, (unsigned)h);
   touch(c, x, y, x + w, y + h);
}

void cv_copy(void *srcp, void *dstp, const sdlgc *gc, int x, int y, int w, int h, int x2, int y2)
{
   Canvas *src = srcp, *dst = dstp;
   XGCValues v;
   /* clamp to the source */
   if (x < 0) { w += x; x2 -= x; x = 0; }
   if (y < 0) { h += y; y2 -= y; y = 0; }
   if (x + w > src->w) w = src->w - x;
   if (y + h > src->h) h = src->h - y;
   if (w <= 0 || h <= 0)
      return;
   v.function = gc->reverse ? GXxor : gc->drawop;
   v.fill_style = FillSolid;
   XChangeGC(xdpy, dgc, GCFunction | GCFillStyle, &v);
   clip(gc);
   XCopyArea(xdpy, src->pm, dst->pm, dgc, x, y, (unsigned)w, (unsigned)h, x2, y2);
   touch(dst, x2, y2, x2 + w, y2 + h);
}

/* ---------- pixels ---------- */

void cv_read(void *cv, int x, int y, int w, int h, unsigned int bg, unsigned int *out)
{
   Canvas *c = cv;
   int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
   int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
   XImage *im = NULL;
   if (x0 < x1 && y0 < y1)
      im = XGetImage(xdpy, c->pm, x0, y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0), AllPlanes, ZPixmap);
   for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
         int lx = x + i, ly = y + j;
         if (!im || lx < x0 || ly < y0 || lx >= x1 || ly >= y1)
            *out++ = bg & 0xFFFFFF;
         else
            *out++ = x_rgb(XGetPixel(im, lx - x0, ly - y0));
      }
   if (im)
      XDestroyImage(im);
}

/* pixels written as they are (copy mode), where opaque; the clip applies */
void cv_write(void *cv, const sdlgc *gc, int x, int y, int w, int h, const unsigned int *px,
   const unsigned char *opaque)
{
   Canvas *c = cv;
   XImage *im;
   Pixmap mask = None;
   int all = 1;

   if (w <= 0 || h <= 0)
      return;
   im = XCreateImage(xdpy, DefaultVisual(xdpy, DefaultScreen(xdpy)), (unsigned)xdepth, ZPixmap, 0,
      NULL, (unsigned)w, (unsigned)h, 32, 0);
   if (!im)
      return;
   im->data = malloc((size_t)im->bytes_per_line * h);
   if (!im->data) {
      XDestroyImage(im);
      return;
   }
   for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
         XPutPixel(im, i, j, x_pixel(px[j * w + i]));
   if (opaque)
      for (int i = 0; i < w * h && all; i++)
         all = opaque[i] != 0;
   XSetFunction(xdpy, dgc, GXcopy);
   XSetFillStyle(xdpy, dgc, FillSolid);
   if (all)
      clip(gc);
   else {
      /* a mask of the opaque pixels within the clip */
      XImage *mi = XCreateImage(xdpy, DefaultVisual(xdpy, DefaultScreen(xdpy)), 1, XYBitmap, 0, NULL,
         (unsigned)w, (unsigned)h, 8, 0);
      mi->data = calloc((size_t)mi->bytes_per_line, (size_t)h);
      for (int j = 0; j < h; j++)
         for (int i = 0; i < w; i++) {
            int on = opaque[j * w + i] != 0;
            if (gc->clipw >= 0 && (x + i < gc->clipx || y + j < gc->clipy
                || x + i >= gc->clipx + gc->clipw || y + j >= gc->clipy + gc->cliph))
               on = 0;
            XPutPixel(mi, i, j, (unsigned long)on);
         }
      mask = XCreatePixmap(xdpy, xroot, (unsigned)w, (unsigned)h, 1);
      GC g = bitgc(mask);
      XPutImage(xdpy, mask, g, mi, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
      XFreeGC(xdpy, g);
      XDestroyImage(mi);
      XSetClipMask(xdpy, dgc, mask);
      XSetClipOrigin(xdpy, dgc, x, y);
   }
   XPutImage(xdpy, c->pm, dgc, im, 0, 0, x, y, (unsigned)w, (unsigned)h);
   if (mask != None) {
      XSetClipOrigin(xdpy, dgc, 0, 0);
      XSetClipMask(xdpy, dgc, None);
      XFreePixmap(xdpy, mask);
   }
   XDestroyImage(im);
   touch(c, x, y, x + w, y + h);
}

/* ---------- text ---------- */

/* DrawString (image 0), or write()'s text (image 1: XDrawImageString,
 * which fills the text's cell with bg first); returns the width */
int cv_text(void *cv, const sdlgc *gc, void *font, int x, int y, const char *s, int n, int image)
{
   Canvas *c = cv;
   XFontStruct *fs = x_fontstruct(font);
   int w;
   if (!fs || n <= 0)
      return 0;
   use(gc);
   XSetFont(xdpy, dgc, fs->fid);
   if (image)
      XDrawImageString(xdpy, c->pm, dgc, x, y, s, n);
   else
      XDrawString(xdpy, c->pm, dgc, x, y, s, n);
   w = XTextWidth(fs, s, n);
   touch(c, x + (fs->min_bounds.lbearing < 0 ? fs->min_bounds.lbearing : 0) - 1, y - fs->ascent - 1,
      x + w + (fs->max_bounds.rbearing > 0 ? fs->max_bounds.rbearing : 0) + 1, y + fs->descent + 1);
   return w;
}
