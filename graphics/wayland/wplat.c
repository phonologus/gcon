/*
 * wplat.c -- the Wayland display server's platform layer: the display,
 * windows and events (sdlgfx.h's pl_*), with libwayland-client,
 * xkbcommon (keys to X keysyms), wayland-cursor (pointer shapes) and,
 * where it was found at build time, libdecor (title bars where the
 * compositor draws none: GNOME). MIT licence, see ../LICENSE.md.
 *
 * Wayland has no drawing: canvases are ../raster's, and a window shows
 * its canvas through two shared-memory buffers in turn, each brought up
 * to date (the canvas's changed area since that buffer was last shown)
 * before it is committed. One thread: pl_main runs the protocol reader
 * directly; events are read whenever the reader polls, waits, opens or
 * presents (so that pings are answered and buffers come back while a
 * program draws without reading events). Keys repeat here, as Wayland
 * leaves that to clients, at the compositor's rate.
 *
 * What Wayland does not let a client do is not done: placing a window,
 * reading its position (pl_position fails), raising or lowering it,
 * moving the pointer, the whole screen's pointer position.
 */
#define _GNU_SOURCE			/* memfd_create */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#ifdef WITH_LIBDECOR
#include <libdecor.h>
#endif
#include "sdlgfx.h"

/* Linux's evdev codes (linux/input-event-codes.h) */
enum { BTN_LEFT_ = 0x110, BTN_RIGHT_ = 0x111, BTN_MIDDLE_ = 0x112 };

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_pointer *ptr;
static struct wl_keyboard *kbd;
static struct zxdg_decoration_manager_v1 *decoman;
#ifdef WITH_LIBDECOR
static struct libdecor *decor;
#endif

/* the outputs: their scale and size, for the canvas scale and
 * displaywidth/displayheight */
typedef struct Out {
   struct wl_output *o;
   uint32_t name;
   int scale, w, h;
   struct Out *next;
} Out;
static Out *outs;

/* ---------- the connection ---------- */

static void pump(void);

/* 1 if the display opens */
static int w_open(void);

const char *gfx_describe(void)
{
   static char s[64];
#ifdef WITH_LIBDECOR
   snprintf(s, sizeof s, "Wayland (libwayland-client, libdecor)");
#else
   snprintf(s, sizeof s, "Wayland (libwayland-client)");
#endif
   return s;
}

/* ---------- windows ---------- */

/* a shared-memory buffer, and the device rectangle of it that is out of
 * date (stale[0] < 0: none) */
typedef struct {
   struct wl_buffer *b;
   uint32_t *px;
   size_t size;
   int w, h, busy;
   int stale[4];
} Buf;

/* A window's buffers: two usually suffice, one shown and one being
 * drawn, but a compositor may hold on to both for a while, and a
 * present must not wait for events a program that draws and then sleeps
 * never lets the server read (the frame would stay unshown). */
enum { NBUF = 4 };

typedef struct Win {
   struct wl_surface *surf;
   struct xdg_surface *xs;
   struct xdg_toplevel *top;
   struct zxdg_toplevel_decoration_v1 *deco;
#ifdef WITH_LIBDECOR
   struct libdecor_frame *frame;
#endif
   void *cv;			/* the canvas shown */
   int scale;
   int lw, lh;			/* its logical size */
   int resizable, state, configured, exposed, pending, awaiting, cursor;
   int cfw, cfh, cfmax;		/* the configure being made */
   Buf buf[NBUF];
   int ov, o[4];		/* the overlay: device rectangle */
   unsigned int ovrgb;
   struct Win *next;
} Win;

static Win *wins;

static Win *find(struct wl_surface *s)
{
   for (Win *w = wins; w; w = w->next)
      if (w->surf == s)
         return w;
   return NULL;
}

/* the queue of translated events */
typedef struct Ev { pl_event e; struct Ev *next; } Ev;
static Ev *qhead, *qtail;

static void enqueue(const pl_event *e)
{
   Ev *n = malloc(sizeof *n);
   if (!n)
      return;
   n->e = *e;
   n->next = NULL;
   if (qtail)
      qtail->next = n;
   else
      qhead = n;
   qtail = n;
}

static void post(Win *w, int kind, long code, int x, int y)
{
   pl_event e;
   memset(&e, 0, sizeof e);
   e.win = w;
   e.kind = kind;
   e.code = code;
   e.x = x;
   e.y = y;
   enqueue(&e);
}

/* r := r U s; a rectangle x0, y0, x1, y1, r[0] < 0 if empty */
static void join(int r[4], const int s[4])
{
   if (s[0] >= s[2] || s[1] >= s[3])
      return;
   if (r[0] < 0) {
      memcpy(r, s, 4 * sizeof *r);
      return;
   }
   if (s[0] < r[0]) r[0] = s[0];
   if (s[1] < r[1]) r[1] = s[1];
   if (s[2] > r[2]) r[2] = s[2];
   if (s[3] > r[3]) r[3] = s[3];
}

static void stale(Win *w, const int r[4])
{
   for (int i = 0; i < NBUF; i++)
      join(w->buf[i].stale, r);
}

static void buf_release(void *data, struct wl_buffer *b);
static const struct wl_buffer_listener buf_listener = { buf_release };

static void buf_free(Buf *b)
{
   if (b->b)
      wl_buffer_destroy(b->b);
   if (b->px)
      munmap(b->px, b->size);
   memset(b, 0, sizeof *b);
}

/* a buffer of w x h device pixels, as XRGB8888 (the canvas's 0xFFRRGGBB) */
static int buf_new(Win *win, Buf *b, int w, int h)
{
   size_t size = (size_t)w * (size_t)h * 4;
   int fd = memfd_create("gcon-graphics", MFD_CLOEXEC);
   struct wl_shm_pool *pool;
   if (fd < 0)
      return 0;
   if (ftruncate(fd, (off_t)size) < 0) {
      close(fd);
      return 0;
   }
   b->px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (b->px == MAP_FAILED) {
      b->px = NULL;
      close(fd);
      return 0;
   }
   pool = wl_shm_create_pool(shm, fd, (int32_t)size);
   b->b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);
   wl_buffer_add_listener(b->b, &buf_listener, win);
   b->size = size;
   b->w = w;
   b->h = h;
   b->busy = 0;
   b->stale[0] = 0; b->stale[1] = 0; b->stale[2] = w; b->stale[3] = h;
   return 1;
}

/* show the canvas: bring a free buffer up to date and commit it; if
 * both are with the compositor, when one comes back */
static void await(Win *w, int ms);

static void show(Win *w)
{
   unsigned char *px;
   long stride;
   int pw, ph, d[4] = { -1, 0, 0, 0 };
   Buf *b = NULL;

   if (!w->cv)
      return;
   if (!w->configured) {
      w->pending = 1;
      return;
   }
   /* a free buffer, else a new one, else wait a little for one */
   for (int tries = 0; !b && tries < 2; tries++) {
      for (int i = 0; i < NBUF && !b; i++)
         if (w->buf[i].b && !w->buf[i].busy)
            b = &w->buf[i];
      for (int i = 0; i < NBUF && !b; i++)
         if (!w->buf[i].b)
            b = &w->buf[i];
      if (!b && tries == 0)
         await(w, 100);
   }
   if (!b) {
      w->pending = 1;		/* shown when one comes back */
      return;
   }
   w->pending = 0;
   cv_pixels(w->cv, &px, &stride, &pw, &ph);
   if (b->w != pw || b->h != ph) {
      buf_free(b);
      if (!buf_new(w, b, pw, ph))
         return;
   }
   if (b->stale[0] >= 0) {
      int *s = b->stale;
      if (s[0] < 0) s[0] = 0;
      if (s[1] < 0) s[1] = 0;
      if (s[2] > pw) s[2] = pw;
      if (s[3] > ph) s[3] = ph;
      for (int y = s[1]; y < s[3]; y++)
         memcpy(b->px + (size_t)y * (size_t)pw + s[0], px + y * stride + s[0] * 4,
                (size_t)(s[2] - s[0]) * 4);
      join(d, s);
      b->stale[0] = -1;
   }
   if (w->ov) {
      int x0 = w->o[0] < 0 ? 0 : w->o[0], y0 = w->o[1] < 0 ? 0 : w->o[1];
      int x1 = w->o[2] > pw ? pw : w->o[2], y1 = w->o[3] > ph ? ph : w->o[3];
      for (int y = y0; y < y1; y++)
         for (int x = x0; x < x1; x++)
            b->px[(size_t)y * (size_t)pw + x] = 0xFF000000u | w->ovrgb;
      join(d, w->o);
      join(b->stale, w->o);		/* the canvas's, when it goes */
   }
   wl_surface_attach(w->surf, b->b, 0, 0);
   wl_surface_set_buffer_scale(w->surf, w->scale);
   if (d[0] >= 0)
      wl_surface_damage_buffer(w->surf, d[0], d[1], d[2] - d[0], d[3] - d[1]);
   b->busy = 1;
   wl_surface_commit(w->surf);
   wl_display_flush(dpy);
}

static void buf_release(void *data, struct wl_buffer *wb)
{
   Win *w = data;
   for (int i = 0; i < NBUF; i++)
      if (w->buf[i].b == wb)
         w->buf[i].busy = 0;
   if (w->pending && !w->awaiting)
      show(w);
}

/* await reads events for up to ms milliseconds, until one of w's
 * buffers comes back */
static void await(Win *w, int ms)
{
   w->awaiting = 1;
   for (int left = ms; left > 0; left -= 10) {
      for (int i = 0; i < NBUF; i++)
         if (w->buf[i].b && !w->buf[i].busy)
            left = 0;
      if (left <= 0)
         break;
#ifdef WITH_LIBDECOR
      if (decor) {
         libdecor_dispatch(decor, 10);
         continue;
      }
#endif
      while (wl_display_prepare_read(dpy) != 0)
         wl_display_dispatch_pending(dpy);
      wl_display_flush(dpy);
      struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
      if (poll(&p, 1, 10) > 0)
         wl_display_read_events(dpy);
      else
         wl_display_cancel_read(dpy);
      wl_display_dispatch_pending(dpy);
   }
   w->awaiting = 0;
}

/* the size bounds: fixed unless resizable */
static void bounds(Win *w)
{
   int mw = w->resizable ? 0 : w->lw, mh = w->resizable ? 0 : w->lh;
#ifdef WITH_LIBDECOR
   if (w->frame) {
      libdecor_frame_set_min_content_size(w->frame, w->resizable ? 1 : mw, w->resizable ? 1 : mh);
      libdecor_frame_set_max_content_size(w->frame, mw, mh);
      if (w->resizable)
         libdecor_frame_set_capabilities(w->frame, LIBDECOR_ACTION_RESIZE);
      else
         libdecor_frame_unset_capabilities(w->frame, LIBDECOR_ACTION_RESIZE);
      return;
   }
#endif
   if (w->top) {
      xdg_toplevel_set_min_size(w->top, mw, mh);
      xdg_toplevel_set_max_size(w->top, mw, mh);
   }
}

/* a configure: the size and state the compositor would have; the
 * first exposes the window */
static void configured(Win *w, int cw, int ch, int max)
{
   int st = max ? PL_MAXIMAL : w->state == PL_ICONIC ? PL_ICONIC : PL_NORMAL;
   if (!w->configured) {
      w->configured = 1;
      w->exposed = 1;
      post(w, PL_EXPOSE, 0, 0, 0);
   }
   if (cw > 0 && ch > 0 && (cw != w->lw || ch != w->lh) && (w->resizable || max)) {
      w->lw = cw;
      w->lh = ch;
      post(w, PL_RESIZE, 0, cw, ch);
   }
   if ((st == PL_MAXIMAL) != (w->state == PL_MAXIMAL)) {
      w->state = st;
      post(w, PL_STATE, st, 0, 0);
   }
   show(w);
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
   Win *w = data;
   xdg_surface_ack_configure(xs, serial);
   configured(w, w->cfw, w->cfh, w->cfmax);
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t cw, int32_t ch,
   struct wl_array *states)
{
   Win *w = data;
   uint32_t *s;
   (void)t;
   w->cfw = cw;
   w->cfh = ch;
   w->cfmax = 0;
   wl_array_for_each(s, states)
      if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED || *s == XDG_TOPLEVEL_STATE_FULLSCREEN)
         w->cfmax = 1;
}
static void top_close(void *data, struct xdg_toplevel *t)
{
   (void)t;
   post(data, PL_CLOSE, 0, 0, 0);
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = {
   .configure = top_configure, .close = top_close,
   .configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

#ifdef WITH_LIBDECOR
static void fr_configure(struct libdecor_frame *f, struct libdecor_configuration *c, void *data)
{
   Win *w = data;
   int cw = 0, ch = 0, max = 0;
   enum libdecor_window_state ws;
   struct libdecor_state *st;
   if (!libdecor_configuration_get_content_size(c, f, &cw, &ch))
      cw = ch = 0;
   if (libdecor_configuration_get_window_state(c, &ws))
      max = (ws & (LIBDECOR_WINDOW_STATE_MAXIMIZED | LIBDECOR_WINDOW_STATE_FULLSCREEN)) != 0;
   if (cw <= 0 || ch <= 0 || !(w->resizable || max)) {
      cw = w->lw;
      ch = w->lh;
   }
   st = libdecor_state_new(cw, ch);
   libdecor_frame_commit(f, st, c);
   libdecor_state_free(st);
   configured(w, cw, ch, max);
}
static void fr_close(struct libdecor_frame *f, void *data)
{
   (void)f;
   post(data, PL_CLOSE, 0, 0, 0);
}
static void fr_commit(struct libdecor_frame *f, void *data)
{
   Win *w = data;
   (void)f;
   wl_surface_commit(w->surf);
}
static struct libdecor_frame_interface fr_iface = {
   .configure = fr_configure, .close = fr_close, .commit = fr_commit,
};
static void decor_error(struct libdecor *d, enum libdecor_error e, const char *msg)
{
   (void)d; (void)e;
   fprintf(stderr, "gcon-graphics-wayland: libdecor: %s\n", msg);
}
static struct libdecor_interface decor_iface = { .error = decor_error };
#endif

/* the scale for a new window: ICONSDL_SCALE, else the outputs' largest */
static int outscale(void)
{
   const char *e = getenv("ICONSDL_SCALE");
   int s = 1;
   if (e && atoi(e) > 0)
      return atoi(e);
   for (Out *o = outs; o; o = o->next)
      if (o->scale > s)
         s = o->scale;
   return s;
}

static void inject(Win *w, const char *script);

void *pl_open(const char *title, int x, int y, int w, int h, int state,
   int resizable, int *scale)
{
   static int injected;
   Win *n;

   (void)x; (void)y;			/* Wayland places windows */
   *scale = 1;
   if (!w_open())
      return NULL;
   if (!(n = calloc(1, sizeof *n)))
      return NULL;
   for (int i = 0; i < NBUF; i++)
      n->buf[i].stale[0] = -1;
   n->scale = *scale = outscale();
   n->lw = w;
   n->lh = h;
   n->resizable = resizable;
   n->state = PL_NORMAL;
   n->surf = wl_compositor_create_surface(comp);
   n->next = wins;
   wins = n;
#ifdef WITH_LIBDECOR
   if (decor) {
      n->frame = libdecor_decorate(decor, n->surf, &fr_iface, n);
      libdecor_frame_set_app_id(n->frame, "gcon");
      libdecor_frame_set_title(n->frame, title);
      bounds(n);
      libdecor_frame_map(n->frame);
   } else
#endif
   {
      n->xs = xdg_wm_base_get_xdg_surface(wm, n->surf);
      xdg_surface_add_listener(n->xs, &xs_listener, n);
      n->top = xdg_surface_get_toplevel(n->xs);
      xdg_toplevel_add_listener(n->top, &top_listener, n);
      xdg_toplevel_set_app_id(n->top, "gcon");
      xdg_toplevel_set_title(n->top, title);
      if (decoman) {
         n->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decoman, n->top);
         zxdg_toplevel_decoration_v1_set_mode(n->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
      }
      bounds(n);
      wl_surface_commit(n->surf);
   }
   /* the first configure, before anything is shown */
   for (int i = 0; i < 100 && !n->configured; i++) {
      wl_display_flush(dpy);
#ifdef WITH_LIBDECOR
      if (decor) {
         if (libdecor_dispatch(decor, 50) < 0)
            break;
         continue;
      }
#endif
      struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
      while (wl_display_prepare_read(dpy) != 0)
         wl_display_dispatch_pending(dpy);
      if (poll(&p, 1, 50) > 0)
         wl_display_read_events(dpy);
      else
         wl_display_cancel_read(dpy);
      wl_display_dispatch_pending(dpy);
   }
   if (state == PL_MAXIMAL || state == PL_ICONIC)
      pl_state(n, state);
   if (!injected) {
      const char *s = getenv("ICONSDL_TEST_EVENTS");
      injected = 1;
      if (s && *s)
         inject(n, s);
   }
   return n;
}

void pl_close(void *win)
{
   Win *w = win;
   for (Win **p = &wins; *p; p = &(*p)->next)
      if (*p == w) {
         *p = w->next;
         break;
      }
   for (Ev **p = &qhead; *p;) {		/* drop its queued events */
      if ((*p)->e.win == w) {
         Ev *d = *p;
         *p = d->next;
         free(d);
      } else
         p = &(*p)->next;
   }
   qtail = NULL;
   for (Ev *e = qhead; e; e = e->next)
      qtail = e;
#ifdef WITH_LIBDECOR
   if (w->frame)
      libdecor_frame_unref(w->frame);
#endif
   if (w->deco)
      zxdg_toplevel_decoration_v1_destroy(w->deco);
   if (w->top)
      xdg_toplevel_destroy(w->top);
   if (w->xs)
      xdg_surface_destroy(w->xs);
   wl_surface_destroy(w->surf);
   for (int i = 0; i < NBUF; i++)
      buf_free(&w->buf[i]);
   wl_display_flush(dpy);
   free(w);
}

void pl_show(void *win) { (void)win; }

void pl_present(void *win, void *canvas, int lw, int lh, int full)
{
   Win *w = win;
   int r[4];
   (void)lw; (void)lh;
   if (!w || !canvas)
      return;
   if (w->cv != canvas)
      full = 1;
   w->cv = canvas;
   if (cv_take_dirty(canvas, r))
      stale(w, r);
   if (full) {
      unsigned char *px;
      long stride;
      int pw, ph;
      cv_pixels(canvas, &px, &stride, &pw, &ph);
      int all[4] = { 0, 0, pw, ph };
      stale(w, all);
   }
   if (w->ov) {				/* the overlay goes */
      w->ov = 0;
      stale(w, w->o);
   }
   show(w);
   pump();
}

void pl_title(void *win, const char *s)
{
   Win *w = win;
#ifdef WITH_LIBDECOR
   if (w->frame) {
      libdecor_frame_set_title(w->frame, s);
      return;
   }
#endif
   xdg_toplevel_set_title(w->top, s);
   wl_display_flush(dpy);
}

void pl_geometry(void *win, int x, int y, int w, int h)
{
   Win *p = win;
   (void)x; (void)y;
   if (w > 0 && h > 0) {
      p->lw = w;
      p->lh = h;
      bounds(p);
#ifdef WITH_LIBDECOR
      if (p->frame && p->configured) {
         struct libdecor_state *st = libdecor_state_new(w, h);
         libdecor_frame_commit(p->frame, st, NULL);
         libdecor_state_free(st);
      }
#endif
   }
}

int pl_position(void *win, int *x, int *y)
{
   (void)win;
   *x = *y = 0;
   return 0;				/* Wayland does not say */
}

void pl_state(void *win, int state)
{
   Win *w = win;
#ifdef WITH_LIBDECOR
   if (w->frame) {
      if (state == PL_ICONIC)
         libdecor_frame_set_minimized(w->frame);
      else if (state == PL_MAXIMAL)
         libdecor_frame_set_maximized(w->frame);
      else if (state == PL_NORMAL && w->state == PL_MAXIMAL)
         libdecor_frame_unset_maximized(w->frame);
   } else
#endif
   {
      if (state == PL_ICONIC)
         xdg_toplevel_set_minimized(w->top);
      else if (state == PL_MAXIMAL)
         xdg_toplevel_set_maximized(w->top);
      else if (state == PL_NORMAL && w->state == PL_MAXIMAL)
         xdg_toplevel_unset_maximized(w->top);
   }
   if (state == PL_ICONIC)
      w->state = PL_ICONIC;		/* Wayland never says it was */
   wl_display_flush(dpy);
}

void pl_resizable(void *win, int on)
{
   Win *w = win;
   w->resizable = on;
   bounds(w);
   wl_display_flush(dpy);
}

void pl_raise(void *win) { (void)win; }
void pl_lower(void *win) { (void)win; }
void pl_warp(void *win, int x, int y) { (void)win; (void)x; (void)y; }
void pl_icon(void *win, const unsigned int *px, int w, int h)
{ (void)win; (void)px; (void)w; (void)h; }

void pl_beep(int volume)
{
   (void)volume;			/* no bell in Wayland's core */
}

void pl_overlay(void *win, int x, int y, int w, int h, unsigned int rgb, int scale)
{
   Win *p = win;
   if (p->ov)
      stale(p, p->o);
   p->ov = 1;
   p->ovrgb = rgb & 0xFFFFFF;
   p->o[0] = x * scale;
   p->o[1] = y * scale;
   p->o[2] = (x + w) * scale;
   p->o[3] = (y + h) * scale;
   show(p);
}

void pl_display_size(void *win, int *w, int *h)
{
   (void)win;
   *w = *h = 0;
   if (!w_open())
      return;
   for (Out *o = outs; o; o = o->next)
      if (o->w > 0) {
         int s = o->scale > 0 ? o->scale : 1;
         *w = o->w / s;
         *h = o->h / s;
         return;
      }
}

int pl_global_mouse(int *x, int *y)
{
   *x = *y = 0;
   return 0;				/* Wayland does not say */
}

/* ---------- the pointer ---------- */

static struct wl_cursor_theme *theme;
static struct wl_surface *cursurf;
static int curscale;
static Win *pwin;			/* the window the pointer is in */
static uint32_t pserial;		/* its enter's serial */
static int px_, py_;			/* where, in it */
static int buttons;			/* held: 1, 2, 4 */

static void setcursor(Win *w)
{
   static const struct { int plc; const char *css, *x; } map[] = {
      { PLC_DEFAULT, "default", "left_ptr" }, { PLC_TEXT, "text", "xterm" },
      { PLC_WAIT, "wait", "watch" }, { PLC_CROSSHAIR, "crosshair", "crosshair" },
      { PLC_NS, "ns-resize", "sb_v_double_arrow" }, { PLC_EW, "ew-resize", "sb_h_double_arrow" },
      { PLC_MOVE, "move", "fleur" }, { PLC_NO, "not-allowed", "X_cursor" },
      { PLC_POINTER, "pointer", "hand2" }, { PLC_NW, "nw-resize", "top_left_corner" },
      { PLC_N, "n-resize", "top_side" }, { PLC_NE, "ne-resize", "top_right_corner" },
      { PLC_E, "e-resize", "right_side" }, { PLC_SE, "se-resize", "bottom_right_corner" },
      { PLC_S, "s-resize", "bottom_side" }, { PLC_SW, "sw-resize", "bottom_left_corner" },
      { PLC_W, "w-resize", "left_side" },
   };
   struct wl_cursor *c = NULL;
   struct wl_cursor_image *im;
   struct wl_buffer *b;
   int s = w->scale > 0 ? w->scale : 1;

   if (!ptr)
      return;
   if (!theme || curscale != s) {
      const char *e = getenv("XCURSOR_SIZE");
      int size = e && atoi(e) > 0 ? atoi(e) : 24;
      if (theme)
         wl_cursor_theme_destroy(theme);
      theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * s, shm);
      curscale = s;
   }
   if (!theme)
      return;
   for (size_t i = 0; i < sizeof map / sizeof map[0] && !c; i++)
      if (map[i].plc == w->cursor)
         if (!(c = wl_cursor_theme_get_cursor(theme, map[i].css)))
            c = wl_cursor_theme_get_cursor(theme, map[i].x);
   if (!c && !(c = wl_cursor_theme_get_cursor(theme, "default")))
      c = wl_cursor_theme_get_cursor(theme, "left_ptr");
   if (!c || c->image_count < 1)
      return;
   im = c->images[0];
   if (!(b = wl_cursor_image_get_buffer(im)))
      return;
   if (!cursurf)
      cursurf = wl_compositor_create_surface(comp);
   wl_surface_set_buffer_scale(cursurf, s);
   wl_surface_attach(cursurf, b, 0, 0);
   wl_surface_damage_buffer(cursurf, 0, 0, (int32_t)im->width, (int32_t)im->height);
   wl_surface_commit(cursurf);
   wl_pointer_set_cursor(ptr, pserial, cursurf, (int32_t)im->hotspot_x / s, (int32_t)im->hotspot_y / s);
   wl_display_flush(dpy);
}

void pl_cursor(void *win, int cursor)
{
   Win *w = win;
   w->cursor = cursor;
   if (pwin == w)
      setcursor(w);
}

static long xmods(void);

static void pointer_event(int kind, long code, uint32_t time)
{
   pl_event e;
   if (!pwin)
      return;
   memset(&e, 0, sizeof e);
   e.win = pwin;
   e.kind = kind;
   e.code = code;
   e.x = px_;
   e.y = py_;
   e.mods = xmods();
   e.ms = time;
   enqueue(&e);
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
   wl_fixed_t x, wl_fixed_t y)
{
   (void)d; (void)p;
   pwin = find(s);
   pserial = serial;
   px_ = wl_fixed_to_int(x);
   py_ = wl_fixed_to_int(y);
   if (pwin)
      setcursor(pwin);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
   (void)d; (void)p; (void)serial; (void)s;
   pwin = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
   (void)d; (void)p;
   px_ = wl_fixed_to_int(x);
   py_ = wl_fixed_to_int(y);
   pointer_event(PL_MOTION, buttons, time);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
   uint32_t button, uint32_t state)
{
   int b = button == BTN_LEFT_ ? 1 : button == BTN_MIDDLE_ ? 2 : button == BTN_RIGHT_ ? 3 : 0;
   int bit = b == 3 ? 4 : b;
   (void)d; (void)p; (void)serial;
   if (!b)
      return;
   if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      pointer_event(PL_PRESS, b, time);
      buttons |= bit;
   } else {
      buttons &= ~bit;
      pointer_event(PL_RELEASE, b, time);
   }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }	/* the wheel, as the others ignore it */
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener p_listener = {
   .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button,
   .axis = p_axis, .frame = p_frame, .axis_source = p_axis_source,
   .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

/* ---------- the keyboard ---------- */

static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *kstate;
static Win *kwin;			/* the window with the keyboard */
static int rep_rate = 25, rep_delay = 600;
static uint32_t rep_key;		/* a key repeating (an evdev code + 8), or 0 */
static struct timespec rep_next;

/* a default keymap (the environment's XKB_DEFAULT_*, else "us"), where
 * the compositor has sent none: for scripted keys */
static int defkeymap(void)
{
   struct xkb_rule_names names;
   if (keymap)
      return 1;
   if (!xkb && !(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
      return 0;
   memset(&names, 0, sizeof names);
   if (!(keymap = xkb_keymap_new_from_names(xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS)))
      return 0;
   kstate = xkb_state_new(keymap);
   return kstate != NULL;
}

static long modsof(struct xkb_state *s)
{
   long m = 0;
   if (!s)
      return 0;
   if (xkb_state_mod_name_is_active(s, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
      m |= 1;				/* X's ShiftMask */
   if (xkb_state_mod_name_is_active(s, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
      m |= 4;				/* ControlMask */
   if (xkb_state_mod_name_is_active(s, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
      m |= 8;				/* Mod1Mask */
   return m;
}

static long xmods(void) { return modsof(kstate); }

static int modifier(xkb_keysym_t ks)
{
   return (ks >= XKB_KEY_Shift_L && ks <= XKB_KEY_Hyper_R)
      || (ks >= XKB_KEY_ISO_Lock && ks <= XKB_KEY_ISO_Level5_Lock)
      || ks == XKB_KEY_Mode_switch || ks == XKB_KEY_Num_Lock;
}

/* A key press as Icon's X11 backend reports it (XLookupString): its
 * Latin-1 character, shift and control applied, else its keysym;
 * modifier keys alone report nothing. */
static void keypress(Win *w, struct xkb_state *s, xkb_keycode_t code, uint32_t time)
{
   xkb_keysym_t ks = xkb_state_key_get_one_sym(s, code);
   uint32_t c;
   pl_event e;
   if (ks == XKB_KEY_NoSymbol || modifier(ks) || !w)
      return;
   memset(&e, 0, sizeof e);
   e.win = w;
   c = xkb_state_key_get_utf32(s, code);
   if (c > 0 && c < 256) {
      e.kind = PL_KEYCHAR;
      e.code = (long)c;
   } else {
      e.kind = PL_KEYSYM;
      e.code = (long)ks;
   }
   if (w == pwin) {
      e.x = px_;
      e.y = py_;
   }
   e.mods = modsof(s);
   e.ms = time;
   enqueue(&e);
}

static unsigned long now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (unsigned long)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
}

static void rep_at(int ms)
{
   clock_gettime(CLOCK_MONOTONIC, &rep_next);
   rep_next.tv_sec += ms / 1000;
   rep_next.tv_nsec += (long)(ms % 1000) * 1000000;
   if (rep_next.tv_nsec >= 1000000000) {
      rep_next.tv_sec++;
      rep_next.tv_nsec -= 1000000000;
   }
}

/* ms until the next repeat (0: due), or -1 if none */
static int rep_due(void)
{
   struct timespec t;
   long ms;
   if (!rep_key || rep_rate <= 0)
      return -1;
   clock_gettime(CLOCK_MONOTONIC, &t);
   ms = (rep_next.tv_sec - t.tv_sec) * 1000 + (rep_next.tv_nsec - t.tv_nsec) / 1000000;
   return ms < 0 ? 0 : (int)ms;
}

static void repeat(void)
{
   while (rep_due() == 0) {
      keypress(kwin, kstate, rep_key, (uint32_t)now_ms());
      rep_at(1000 / rep_rate > 0 ? 1000 / rep_rate : 1);
   }
}

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
   char *s;
   struct xkb_keymap *km;
   (void)d; (void)k;
   if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
      close(fd);
      return;
   }
   s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   if (s == MAP_FAILED)
      return;
   if (!xkb)
      xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
   km = xkb ? xkb_keymap_new_from_string(xkb, s, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
   munmap(s, size);
   if (!km)
      return;
   if (kstate)
      xkb_state_unref(kstate);
   if (keymap)
      xkb_keymap_unref(keymap);
   keymap = km;
   kstate = xkb_state_new(keymap);
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
   struct wl_array *keys)
{
   (void)d; (void)k; (void)serial; (void)keys;
   kwin = find(s);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
   (void)d; (void)k; (void)serial; (void)s;
   kwin = NULL;
   rep_key = 0;
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
   uint32_t state)
{
   xkb_keycode_t code = key + 8;
   (void)d; (void)k; (void)serial;
   if (!kstate)
      return;
   if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      keypress(kwin, kstate, code, time);
      if (xkb_keymap_key_repeats(keymap, code) && rep_rate > 0) {
         rep_key = code;
         rep_at(rep_delay);
      }
   } else if (code == rep_key)
      rep_key = 0;
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
   uint32_t lock, uint32_t group)
{
   (void)d; (void)k; (void)serial;
   if (kstate)
      xkb_state_update_mask(kstate, dep, lat, lock, 0, 0, group);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
   (void)d; (void)k;
   rep_rate = rate;
   rep_delay = delay;
}
static const struct wl_keyboard_listener k_listener = {
   .keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key,
   .modifiers = k_mods, .repeat_info = k_repeat,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
   (void)d;
   if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
      ptr = wl_seat_get_pointer(s);
      wl_pointer_add_listener(ptr, &p_listener, NULL);
   } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ptr) {
      wl_pointer_destroy(ptr);
      ptr = NULL;
      pwin = NULL;
   }
   if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) {
      kbd = wl_seat_get_keyboard(s);
      wl_keyboard_add_listener(kbd, &k_listener, NULL);
   } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && kbd) {
      wl_keyboard_destroy(kbd);
      kbd = NULL;
      kwin = NULL;
      rep_key = 0;
   }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---------- outputs and globals ---------- */

static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
   int32_t sub, const char *make, const char *model, int32_t t)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)t; }
static void o_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
   Out *p = d;
   (void)o; (void)r;
   if (flags & WL_OUTPUT_MODE_CURRENT) {
      p->w = w;
      p->h = h;
   }
}
static void o_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void o_scale(void *d, struct wl_output *o, int32_t s)
{
   (void)o;
   ((Out *)d)->scale = s;
}
static void o_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void o_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener o_listener = {
   .geometry = o_geometry, .mode = o_mode, .done = o_done, .scale = o_scale,
   .name = o_name, .description = o_desc,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
   (void)d;
   xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
   (void)d;
   if (!strcmp(iface, wl_compositor_interface.name))
      comp = wl_registry_bind(r, name, &wl_compositor_interface, min(v, 4));
   else if (!strcmp(iface, wl_shm_interface.name))
      shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
   else if (!strcmp(iface, xdg_wm_base_interface.name)) {
      wm = wl_registry_bind(r, name, &xdg_wm_base_interface, min(v, 4));
      xdg_wm_base_add_listener(wm, &wm_listener, NULL);
   } else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
      seat = wl_registry_bind(r, name, &wl_seat_interface, min(v, 5));
      wl_seat_add_listener(seat, &seat_listener, NULL);
   } else if (!strcmp(iface, wl_output_interface.name)) {
      Out *o = calloc(1, sizeof *o);
      if (!o)
         return;
      o->name = name;
      o->scale = 1;
      o->o = wl_registry_bind(r, name, &wl_output_interface, min(v, 4));
      wl_output_add_listener(o->o, &o_listener, o);
      o->next = outs;
      outs = o;
   } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
      decoman = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name)
{
   (void)d; (void)r;
   for (Out **p = &outs; *p; p = &(*p)->next)
      if ((*p)->name == name) {
         Out *o = *p;
         *p = o->next;
         wl_output_destroy(o->o);
         free(o);
         return;
      }
}
static const struct wl_registry_listener reg_listener = { global, global_remove };

#ifdef WITH_LIBDECOR
/* GCONLIBDECOR=off: no title bars of libdecor's (the compositor's own,
 * where it draws them) */
static int libdecor_off(void)
{
   const char *s = getenv("GCONLIBDECOR");
   return s && !strcmp(s, "off");
}
#endif

static int w_open(void)
{
   static int tried;
   if (dpy || tried)
      return dpy != NULL;
   tried = 1;
   if (!(dpy = wl_display_connect(NULL)))
      return 0;
   reg = wl_display_get_registry(dpy);
   wl_registry_add_listener(reg, &reg_listener, NULL);
   wl_display_roundtrip(dpy);		/* the globals */
   wl_display_roundtrip(dpy);		/* the outputs' and seat's own */
   if (!comp || !shm || !wm) {
      fprintf(stderr, "gcon-graphics-wayland: the compositor lacks %s\n",
         !comp ? "wl_compositor" : !shm ? "wl_shm" : "xdg_wm_base");
      wl_display_disconnect(dpy);
      dpy = NULL;
      return 0;
   }
#ifdef WITH_LIBDECOR
   /* libdecor where the compositor will not decorate */
   if (!decoman && !libdecor_off())
      decor = libdecor_new(dpy, &decor_iface);
#endif
   return 1;
}

/* ---------- events ---------- */

/* read and dispatch what has arrived, without waiting */
static void pump(void)
{
   if (!dpy)
      return;
#ifdef WITH_LIBDECOR
   if (decor) {
      libdecor_dispatch(decor, 0);
      repeat();
      return;
   }
#endif
   while (wl_display_prepare_read(dpy) != 0)
      wl_display_dispatch_pending(dpy);
   wl_display_flush(dpy);
   struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
   if (poll(&p, 1, 0) > 0)
      wl_display_read_events(dpy);
   else
      wl_display_cancel_read(dpy);
   wl_display_dispatch_pending(dpy);
   repeat();
}

int pl_poll(pl_event *e)
{
   pump();
   if (!qhead)
      return 0;
   Ev *h = qhead;
   *e = h->e;
   qhead = h->next;
   if (!qhead)
      qtail = NULL;
   free(h);
   return 1;
}

int pl_wait(int timeout_ms)
{
   int r;
   if (qhead)
      return 1;
   if (!dpy) {
      poll(NULL, 0, timeout_ms);
      return 0;
   }
   pump();
   if (qhead)
      return 1;
   r = rep_due();
   if (r >= 0 && (timeout_ms < 0 || r < timeout_ms))
      timeout_ms = r;
#ifdef WITH_LIBDECOR
   if (decor) {
      libdecor_dispatch(decor, timeout_ms);
      repeat();
      return qhead != NULL;
   }
#endif
   while (wl_display_prepare_read(dpy) != 0)
      wl_display_dispatch_pending(dpy);
   wl_display_flush(dpy);
   struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
   if (poll(&p, 1, timeout_ms) > 0)
      wl_display_read_events(dpy);
   else
      wl_display_cancel_read(dpy);
   wl_display_dispatch_pending(dpy);
   repeat();
   return qhead != NULL;
}

/* ---------- scripted input, for tests ---------- */

/*
 * ICONSDL_TEST_EVENTS, as the other servers read it (../src/plat.c):
 * when the first window opens, its items are queued as if typed or
 * clicked in that window. Keys go through the keymap (the compositor's,
 * else xkbcommon's default) as typed ones do.
 *
 *   key:NAME          NAME a keysym name ("a", "Return", "Left", "F1"),
 *                     optionally after shift+, ctrl+, alt+ or cmd+
 *   down:B:X:Y        mouse button B pressed at X, Y
 *   up:B:X:Y          and released
 *   move:B:X:Y        pointer moved with button B held (0: none)
 *   close             the window's close box
 */
static xkb_keysym_t keysym_named(const char *name)
{
   static const struct { const char *sdl, *x; } alias[] = {
      { "Backspace", "BackSpace" }, { "Space", "space" }, { "Enter", "Return" },
      { "PageUp", "Prior" }, { "PageDown", "Next" },
   };
   xkb_keysym_t ks = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
   if (ks != XKB_KEY_NoSymbol)
      return ks;
   for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
      if (!strcasecmp(name, alias[i].sdl))
         return xkb_keysym_from_name(alias[i].x, XKB_KEYSYM_NO_FLAGS);
   return XKB_KEY_NoSymbol;
}

/* the keycode whose first level in the first layout is ks */
static xkb_keycode_t keycode_of(xkb_keysym_t ks)
{
   xkb_keycode_t lo = xkb_keymap_min_keycode(keymap), hi = xkb_keymap_max_keycode(keymap);
   for (xkb_keycode_t k = lo; k <= hi; k++) {
      const xkb_keysym_t *syms;
      int n = xkb_keymap_key_get_syms_by_level(keymap, k, 0, 0, &syms);
      for (int i = 0; i < n; i++)
         if (syms[i] == ks)
            return k;
   }
   return 0;
}

static void inject_key(Win *w, char *name)
{
   xkb_mod_mask_t m = 0;
   xkb_keysym_t ks;
   xkb_keycode_t code;
   struct xkb_state *s;
   for (;;) {
      if (!strncmp(name, "shift+", 6)) { m |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT); name += 6; }
      else if (!strncmp(name, "ctrl+", 5)) { m |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL); name += 5; }
      else if (!strncmp(name, "alt+", 4) || !strncmp(name, "cmd+", 4)) {
         m |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
         name += 4;
      }
      else break;
   }
   if ((ks = keysym_named(name)) == XKB_KEY_NoSymbol || !(code = keycode_of(ks)))
      return;
   if (!(s = xkb_state_new(keymap)))
      return;
   xkb_state_update_mask(s, m, 0, 0, 0, 0, 0);
   keypress(w, s, code, 0);
   xkb_state_unref(s);
}

static void inject(Win *w, const char *script)
{
   char *s = strdup(script), *save = NULL;
   if (!s)
      return;
   for (char *t = strtok_r(s, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
      pl_event e;
      int b = 0, x = 0, y = 0;
      memset(&e, 0, sizeof e);
      e.win = w;
      if (!strncmp(t, "key:", 4)) {
         if (defkeymap())
            inject_key(w, t + 4);
         continue;
      }
      else if (sscanf(t, "down:%d:%d:%d", &b, &x, &y) == 3 || sscanf(t, "up:%d:%d:%d", &b, &x, &y) == 3) {
         e.kind = t[0] == 'd' ? PL_PRESS : PL_RELEASE;
         e.code = b == 1 ? 1 : b == 2 ? 2 : 3;
      }
      else if (sscanf(t, "move:%d:%d:%d", &b, &x, &y) == 3) {
         e.kind = PL_MOTION;
         e.code = b == 0 ? 0 : b == 1 ? 1 : b == 2 ? 2 : 4;
      }
      else if (!strcmp(t, "close"))
         e.kind = PL_CLOSE;
      else
         continue;
      e.x = x;
      e.y = y;
      enqueue(&e);
   }
   free(s);
}

/* ---------- the thread ---------- */

int pl_main(int (*f)(int, char **), int argc, char **argv)
{
   return f(argc, argv);
}

void pl_shutdown(void)
{
   while (wins)
      pl_close(wins);
   if (dpy) {
      wl_display_flush(dpy);
      wl_display_disconnect(dpy);
      dpy = NULL;
   }
}

/* whether there is a display: whether the compositor answers */
int pl_display(void) { return w_open(); }
