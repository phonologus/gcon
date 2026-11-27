/*
 * main.c -- gcon-graphics, the display server for gcon.
 * MIT licence, see LICENSE.md.
 *
 * Reads requests of the gcon graphics protocol (design/protocol.md) from
 * a socket and carries them out with the canvas and platform layers.
 * The reader runs where icon-sdl ran its interpreter: on the thread
 * pl_main starts, while SDL keeps the main thread (plat.c).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "sdlgfx.h"

enum { VERSION = 2 };

enum {
   HELLO, CANVAS, FREE, RESIZE, GC, POINTS, LINES, FILLPOLY, SEGMENTS,
   RECTS, FILLRECTS, ARCS, ERASE, COPY, WRITE, READ, COLOR, WAIT, SHOW,
   FONT, TEXT, WIDTH, TITLE, GEOMETRY, STATE, MAP, RESIZABLE, POSITION,
   RAISE, LOWER, CURSOR, WARP, BEEP, DISPLAYSIZE, GLOBALMOUSE, OVERLAY, ENCODE,
   DECODE, ICON, NOPS
};

static int fd = -1;

static void fatal(const char *msg)
{
   fprintf(stderr, "gcon-graphics: %s\n", msg);
   exit(2);
}

/* ---------- reading requests ---------- */

static unsigned char *msg;		/* the current request's body */
static size_t msgcap, msglen, msgpos;

static int readfull(void *p, size_t n)
{
   unsigned char *b = p;
   while (n > 0) {
      ssize_t k = read(fd, b, n);
      if (k < 0 && errno == EINTR)
         continue;
      if (k <= 0)
         return 0;
      b += k;
      n -= (size_t)k;
   }
   return 1;
}

/* next reads the next request; 0 at the end of the connection */
static int next(int *op)
{
   unsigned char h[4];
   if (!readfull(h, 4))
      return 0;
   size_t n = h[0] | h[1] << 8 | h[2] << 16 | (size_t)h[3] << 24;
   if (n < 1 || n > (1u << 30))
      fatal("bad message length");
   if (n > msgcap) {
      msg = realloc(msg, n);
      if (!msg)
         fatal("out of memory");
      msgcap = n;
   }
   if (!readfull(msg, n))
      fatal("connection ended inside a message");
   msglen = n;
   msgpos = 1;
   *op = msg[0];
   return 1;
}

static const unsigned char *take(size_t n)
{
   if (msglen - msgpos < n)
      fatal("message too short");
   const unsigned char *p = msg + msgpos;
   msgpos += n;
   return p;
}

static uint32_t u32(void)
{
   const unsigned char *p = take(4);
   return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t i32(void) { return (int32_t)u32(); }
static int u8(void) { return *take(1); }

static double f64(void)
{
   uint64_t v = u32();
   v |= (uint64_t)u32() << 32;
   double d;
   memcpy(&d, &v, 8);
   return d;
}

/* str returns a NUL-terminated copy (malloc'd) of a string */
static char *str(size_t *len)
{
   uint32_t n = u32();
   const unsigned char *p = take(n);
   char *s = malloc((size_t)n + 1);
   if (!s)
      fatal("out of memory");
   memcpy(s, p, n);
   s[n] = 0;
   if (len)
      *len = n;
   return s;
}

/* ints reads n ints into a buffer (valid until the next call) */
static int *ints(size_t n)
{
   static int *b;
   static size_t cap;
   if (n > (msglen - msgpos) / 4)
      fatal("message too short");
   if (n > cap) {
      b = realloc(b, n * sizeof *b);
      if (!b)
         fatal("out of memory");
      cap = n;
   }
   for (size_t i = 0; i < n; i++)
      b[i] = i32();
   return b;
}

/* ---------- writing replies ---------- */

static unsigned char *out;
static size_t outlen, outcap;

static void put(const void *p, size_t n)
{
   if (outlen + n > outcap) {
      outcap = (outlen + n) * 2;
      out = realloc(out, outcap);
      if (!out)
         fatal("out of memory");
   }
   memcpy(out + outlen, p, n);
   outlen += n;
}

static void putu32(uint32_t v)
{
   unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 };
   put(b, 4);
}

static void puti64(int64_t v)
{
   putu32((uint32_t)v);
   putu32((uint32_t)((uint64_t)v >> 32));
}

static void setu32(size_t at, uint32_t v)
{
   out[at] = v; out[at + 1] = v >> 8; out[at + 2] = v >> 16; out[at + 3] = v >> 24;
}

static void begin(int op)
{
   outlen = 0;
   putu32(0);				/* the length, filled in by end */
   unsigned char o = op;
   put(&o, 1);
}

static void end(void)
{
   setu32(0, (uint32_t)(outlen - 4));
   size_t off = 0;
   while (off < outlen) {
      ssize_t k = write(fd, out + off, outlen - off);
      if (k < 0 && errno == EINTR)
         continue;
      if (k <= 0)
         exit(0);			/* the client has gone */
      off += (size_t)k;
   }
}

/* ---------- objects ---------- */

typedef struct Canvas {
   uint32_t id;
   void *cv, *win;
   int w, h;
   struct Canvas *next;
} Canvas;

typedef struct Gc {
   uint32_t id;
   sdlgc g;
   unsigned char *pat;
   struct Gc *next;
} Gc;

typedef struct Fnt {
   uint32_t id;
   void *f;
   struct Fnt *next;
} Fnt;

static Canvas *canvases;
static Gc *gcs;
static Fnt *fonts;

static Canvas *canvas(uint32_t id)
{
   for (Canvas *c = canvases; c; c = c->next)
      if (c->id == id)
         return c;
   fatal("unknown canvas");
   return NULL;
}

static Gc *gc(uint32_t id)
{
   for (Gc *g = gcs; g; g = g->next)
      if (g->id == id)
         return g;
   fatal("unknown gc");
   return NULL;
}

static Fnt *font(uint32_t id)
{
   for (Fnt *f = fonts; f; f = f->next)
      if (f->id == id)
         return f;
   fatal("unknown font");
   return NULL;
}

static void present(int full)
{
   for (Canvas *c = canvases; c; c = c->next)
      if (c->win)
         pl_present(c->win, c->cv, c->w, c->h, full);
}

/* ---------- requests ---------- */

static void do_canvas(void)
{
   uint32_t id = u32();
   int x = i32(), y = i32(), w = i32(), h = i32();
   unsigned bg = u32();
   int state = i32(), resizable = u32();
   char *title = str(NULL);
   int scale = 1, ok = 1;
   Canvas *c = calloc(1, sizeof *c);
   if (!c)
      fatal("out of memory");
   for (Canvas *o = canvases; o; o = o->next)
      if (o->id == id)
         fatal("canvas id in use");
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   if (state != PL_HIDDEN) {
      c->win = pl_open(title, x, y, w, h, state, resizable, &scale);
      ok = c->win != NULL;
   }
   if (ok) {
      c->cv = cv_new(w, h, scale, bg);
      ok = c->cv != NULL;
   }
   free(title);
   begin(CANVAS);
   putu32(ok);
   putu32(scale);
   end();
   if (!ok) {
      if (c->win)
         pl_close(c->win);
      free(c);
      return;
   }
   c->id = id;
   c->w = w;
   c->h = h;
   c->next = canvases;
   canvases = c;
   if (c->win) {
      pl_present(c->win, c->cv, w, h, 1);
      pl_state(c->win, state);
   }
}

static void do_free(void)
{
   uint32_t id = u32();
   for (Canvas **p = &canvases; *p; p = &(*p)->next)
      if ((*p)->id == id) {
         Canvas *c = *p;
         *p = c->next;
         if (c->win)
            pl_close(c->win);
         cv_free(c->cv);
         free(c);
         return;
      }
   fatal("unknown canvas");
}

static void do_resize(void)
{
   Canvas *c = canvas(u32());
   int w = i32(), h = i32();
   unsigned bg = u32();
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   cv_resize(c->cv, w, h, bg);
   c->w = w;
   c->h = h;
   if (c->win)
      pl_present(c->win, c->cv, w, h, 1);
}

static void do_gc(void)
{
   uint32_t id = u32();
   Gc *g = NULL;
   for (Gc *o = gcs; o; o = o->next)
      if (o->id == id)
         g = o;
   if (!g) {
      g = calloc(1, sizeof *g);
      if (!g)
         fatal("out of memory");
      g->id = id;
      g->next = gcs;
      gcs = g;
   }
   sdlgc *s = &g->g;
   s->fg = u32();
   s->bg = u32();
   s->drawop = u8();
   s->reverse = u8();
   s->linewidth = i32();
   s->linestyle = u8();
   s->fillstyle = u8();
   s->clipx = i32();
   s->clipy = i32();
   s->clipw = i32();
   s->cliph = i32();
   s->patw = i32();
   s->path = i32();
   free(g->pat);
   g->pat = NULL;
   if (s->patw > 0 && s->path > 0) {
      size_t n = (size_t)s->patw * s->path;
      g->pat = malloc(n);
      if (!g->pat)
         fatal("out of memory");
      memcpy(g->pat, take(n), n);
   } else
      s->patw = s->path = 0;
   s->pat = g->pat;
}

/* draw handles the requests that are a canvas, a gc and n groups of
 * coordinates */
static void draw(int op)
{
   Canvas *c = canvas(u32());
   Gc *g = gc(u32());
   uint32_t n = u32();
   int k = (op == SEGMENTS || op == RECTS || op == FILLRECTS) ? 4 : 2;
   int *xy = ints((size_t)n * k);
   switch (op) {
   case POINTS: cv_points(c->cv, &g->g, xy, n); break;
   case LINES: cv_lines(c->cv, &g->g, xy, n); break;
   case FILLPOLY: cv_fillpoly(c->cv, &g->g, xy, n); break;
   case SEGMENTS: cv_segments(c->cv, &g->g, xy, n); break;
   case RECTS: cv_rects(c->cv, &g->g, xy, n); break;
   case FILLRECTS: cv_fillrects(c->cv, &g->g, xy, n); break;
   }
}

static void do_arcs(void)
{
   Canvas *c = canvas(u32());
   Gc *g = gc(u32());
   int fill = u8();
   uint32_t n = u32();
   if (n > (msglen - msgpos) / 32)
      fatal("message too short");
   int *xywh = malloc((size_t)n * 4 * sizeof *xywh + 1);
   double *ang = malloc((size_t)n * 2 * sizeof *ang + 1);
   if (!xywh || !ang)
      fatal("out of memory");
   for (uint32_t i = 0; i < n; i++) {
      for (int j = 0; j < 4; j++)
         xywh[4 * i + j] = i32();
      ang[2 * i] = f64();
      ang[2 * i + 1] = f64();
   }
   cv_arcs(c->cv, &g->g, xywh, ang, n, fill);
   free(xywh);
   free(ang);
}

static void do_erase(void)
{
   Canvas *c = canvas(u32());
   Gc *g = gc(u32());
   int x = i32(), y = i32(), w = i32(), h = i32();
   cv_erase(c->cv, &g->g, x, y, w, h);
}

static void do_copy(void)
{
   Canvas *s = canvas(u32()), *d = canvas(u32());
   Gc *g = gc(u32());
   int *a = ints(6);
   cv_copy(s->cv, d->cv, &g->g, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void do_write(void)
{
   Canvas *c = canvas(u32());
   Gc *g = gc(u32());
   int x = i32(), y = i32(), w = i32(), h = i32();
   if (w < 0 || h < 0 || (size_t)w * h > (msglen - msgpos) / 5)
      fatal("message too short");
   size_t n = (size_t)w * h;
   unsigned *px = malloc(n * sizeof *px + 1);
   if (!px)
      fatal("out of memory");
   for (size_t i = 0; i < n; i++)
      px[i] = u32();
   cv_write(c->cv, &g->g, x, y, w, h, px, take(n));
   free(px);
}

static void do_read(void)
{
   Canvas *c = canvas(u32());
   int x = i32(), y = i32(), w = i32(), h = i32();
   unsigned bg = u32();
   if (w < 0 || h < 0 || (size_t)w * h > (1u << 26))
      fatal("bad read size");
   size_t n = (size_t)w * h;
   unsigned *px = malloc(n * sizeof *px + 1);
   if (!px)
      fatal("out of memory");
   cv_read(c->cv, x, y, w, h, bg, px);
   begin(READ);
   for (size_t i = 0; i < n; i++)
      putu32(px[i]);
   end();
   free(px);
}

/* ENCODE: a region of a canvas as a PNG, JPEG, BMP or QOI file's bytes */
static void do_encode(void)
{
   Canvas *c = canvas(u32());
   int x = i32(), y = i32(), w = i32(), h = i32();
   char *e = str(NULL);
   unsigned char *data;
   size_t len;
   int r = cv_encode(c->cv, e, x, y, w, h, &data, &len);
   begin(ENCODE);
   putu32((uint32_t)r);
   putu32((uint32_t)len);
   if (len)
      put(data, len);
   end();
   free(data);
   free(e);
}

/* ICON: a canvas's window's icon, from w x h pixels 0xAARRGGBB */
static void do_icon(void)
{
   Canvas *c = canvas(u32());
   int w = i32(), h = i32();
   if (w <= 0 || h <= 0 || (size_t)w * h > (msglen - msgpos) / 4)
      fatal("message too short");
   size_t n = (size_t)w * h;
   unsigned *px = malloc(n * sizeof *px);
   if (!px)
      fatal("out of memory");
   for (size_t i = 0; i < n; i++)
      px[i] = u32();
   if (c->win)
      pl_icon(c->win, px, w, h);
   free(px);
}

/* DECODE: an image file's bytes as pixels, 0xAARRGGBB */
static void do_decode(void)
{
   size_t len;
   char *b = str(&len);
   int w = 0, h = 0;
   unsigned *px = cv_decode((unsigned char *)b, len, &w, &h);
   begin(DECODE);
   putu32(px != NULL);
   putu32(px ? w : 0);
   putu32(px ? h : 0);
   if (px)
      for (size_t i = 0; i < (size_t)w * h; i++)
         putu32(px[i]);
   end();
   free(px);
   free(b);
}

static void do_color(void)
{
   char *name = str(NULL);
   int r = 0, g = 0, b = 0;
   int ok = xcolor_lookup(name, &r, &g, &b);
   free(name);
   begin(COLOR);
   putu32(ok);
   putu32(r);
   putu32(g);
   putu32(b);
   end();
}

static void do_font(void)
{
   uint32_t id = u32();
   char *family = str(NULL);
   int bold = u8(), italic = u8(), size = i32();
   void *f = ft_open(family, bold, italic, size);
   int a = 0, d = 0, m = 0;
   free(family);
   if (f) {
      for (Fnt *o = fonts; o; o = o->next)
         if (o->id == id)
            fatal("font id in use");
      Fnt *n = calloc(1, sizeof *n);
      if (!n)
         fatal("out of memory");
      n->id = id;
      n->f = f;
      n->next = fonts;
      fonts = n;
      ft_metrics(f, &a, &d, &m);
   }
   begin(FONT);
   putu32(f != NULL);
   putu32(a);
   putu32(d);
   putu32(m);
   end();
}

static void do_text(void)
{
   Canvas *c = canvas(u32());
   Gc *g = gc(u32());
   Fnt *f = font(u32());
   int x = i32(), y = i32(), image = u8();
   size_t n;
   char *s = str(&n);
   cv_text(c->cv, &g->g, f->f, x, y, s, (int)n, image);
   free(s);
}

static void do_width(void)
{
   Fnt *f = font(u32());
   size_t n;
   char *s = str(&n);
   int w = ft_width(f->f, s, (int)n);
   free(s);
   begin(WIDTH);
   putu32(w);
   end();
}

/* window management: requests on a canvas's window (none for a hidden
 * canvas, so these do nothing there) */
static void do_window(int op)
{
   Canvas *c = canvas(u32());
   switch (op) {
   case TITLE: {
      char *t = str(NULL);
      if (c->win)
         pl_title(c->win, t);
      free(t);
      break;
   }
   case GEOMETRY: {
      int x = i32(), y = i32(), w = i32(), h = i32();
      if (c->win) {
         pl_geometry(c->win, x, y, w, h);
         pl_present(c->win, c->cv, c->w, c->h, 1);
      }
      break;
   }
   case STATE: {
      int s = i32();
      if (c->win)
         pl_state(c->win, s);
      break;
   }
   case RESIZABLE: {
      int on = u8();
      if (c->win)
         pl_resizable(c->win, on);
      break;
   }
   case POSITION: {
      int x = 0, y = 0, ok = c->win && pl_position(c->win, &x, &y);
      begin(POSITION);
      putu32(ok);
      putu32(x);
      putu32(y);
      end();
      break;
   }
   case RAISE: if (c->win) pl_raise(c->win); break;
   case LOWER: if (c->win) pl_lower(c->win); break;
   case CURSOR: {
      int k = i32();
      if (c->win)
         pl_cursor(c->win, k);
      break;
   }
   case WARP: {
      int x = i32(), y = i32();
      if (c->win)
         pl_warp(c->win, x, y);
      break;
   }
   case DISPLAYSIZE: {
      int w = 0, h = 0;
      pl_display_size(c->win, &w, &h);
      begin(DISPLAYSIZE);
      putu32(w);
      putu32(h);
      end();
      break;
   }
   case OVERLAY: {
      int x = i32(), y = i32(), w = i32(), h = i32();
      unsigned rgb = u32();
      if (c->win)
         pl_overlay(c->win, x, y, w, h, rgb, cv_scale(c->cv));
      break;
   }
   }
}

/* MAP gives a hidden canvas its window (canvas=normal) */
static void do_map(void)
{
   Canvas *c = canvas(u32());
   int x = i32(), y = i32(), state = i32(), resizable = u8();
   char *title = str(NULL);
   int scale = 1, ok = 1;
   if (!c->win) {
      c->win = pl_open(title, x, y, c->w, c->h, state, resizable, &scale);
      ok = c->win != NULL;
      if (ok) {
         pl_present(c->win, c->cv, c->w, c->h, 1);
         pl_state(c->win, state);
      }
   }
   free(title);
   begin(MAP);
   putu32(ok);
   end();
}

static uint32_t winid(void *win)
{
   for (Canvas *c = canvases; c; c = c->next)
      if (c->win && c->win == win)
         return c->id;
   return 0;
}

/* client_gone: the client has closed the connection. While the server
 * waits for events in WAIT the client sends nothing (it reads the
 * reply first), so the socket becoming readable means it has gone. */
static int client_gone(void)
{
   struct pollfd p = { fd, POLLIN, 0 };
   char c;
   if (poll(&p, 1, 0) <= 0)
      return 0;
   if (p.revents & (POLLHUP | POLLERR))
      return 1;
   return (p.revents & POLLIN) && recv(fd, &c, 1, MSG_PEEK) == 0;
}

static void do_wait(void)
{
   int timeout = i32();
   pl_event e;
   uint32_t n = 0;
   present(0);
   begin(WAIT);
   putu32(0);				/* the count, set below */
   for (int waited = 0;; waited = 1) {
      while (pl_poll(&e)) {
         uint32_t id = winid(e.win);
         if (!id)
            continue;			/* its window is gone */
         putu32(id);
         putu32(e.kind);
         puti64(e.code);
         putu32(e.x);
         putu32(e.y);
         puti64(e.mods);
         puti64((int64_t)e.ms);
         n++;
      }
      if (n > 0 || waited || timeout == 0)
         break;
      /* wait in slices, so that a client gone while it waited (killed,
       * say, while waiting for an event) ends the server: else it
       * would wait for ever */
      for (int left = timeout;;) {
         int slice = left < 0 || left > 100 ? 100 : left;
         if (pl_wait(slice))
            break;
         if (client_gone())
            exit(0);
         if (left >= 0 && (left -= slice) <= 0)
            break;
      }
   }
   setu32(5, n);
   end();
}

static int serve(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   int op;
   while (next(&op)) {
      switch (op) {
      case HELLO: {
         uint32_t v = u32();
         begin(HELLO);
         putu32(VERSION);
         putu32(v == VERSION);
         end();
         break;
      }
      case CANVAS: do_canvas(); break;
      case FREE: do_free(); break;
      case RESIZE: do_resize(); break;
      case GC: do_gc(); break;
      case POINTS: case LINES: case FILLPOLY: case SEGMENTS:
      case RECTS: case FILLRECTS:
         draw(op);
         break;
      case ARCS: do_arcs(); break;
      case ERASE: do_erase(); break;
      case COPY: do_copy(); break;
      case WRITE: do_write(); break;
      case READ: do_read(); break;
      case COLOR: do_color(); break;
      case WAIT: do_wait(); break;
      case SHOW: present(u8()); break;
      case FONT: do_font(); break;
      case TEXT: do_text(); break;
      case WIDTH: do_width(); break;
      case TITLE: case GEOMETRY: case STATE: case RESIZABLE: case POSITION:
      case RAISE: case LOWER: case CURSOR: case WARP: case DISPLAYSIZE:
      case OVERLAY:
         do_window(op);
         break;
      case MAP: do_map(); break;
      case ENCODE: do_encode(); break;
      case DECODE: do_decode(); break;
      case ICON: do_icon(); break;
      case BEEP: pl_beep(i32()); break;
      case GLOBALMOUSE: {
         int x = 0, y = 0, ok = pl_global_mouse(&x, &y);
         begin(GLOBALMOUSE);
         putu32(ok);
         putu32(x);
         putu32(y);
         end();
         break;
      }
      default:
         fatal("unknown request");
      }
      if (msgpos != msglen)
         fatal("message too long");
   }
   while (canvases) {			/* the client has gone */
      Canvas *c = canvases;
      canvases = c->next;
      if (c->win)
         pl_close(c->win);
      cv_free(c->cv);
      free(c);
   }
   pl_shutdown();
   exit(0);
}

int main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "-version") == 0) {
      int v = sdlgfx_probe();
      if (v == 0) {
         fprintf(stderr, "gcon-graphics: SDL3 or Blend2D not working\n");
         return 1;
      }
      printf("gcon-graphics protocol %d; SDL %d.%d.%d\n", VERSION,
         v / 10000, v / 100 % 100, v % 100);
      return 0;
   }
   if (argc == 3 && strcmp(argv[1], "-fd") == 0) {
      fd = atoi(argv[2]);
      return pl_main(serve, argc, argv);
   }
   fprintf(stderr, "usage: gcon-graphics -fd n | -version\n");
   return 2;
}
