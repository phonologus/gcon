/*
 * xplat.c -- the X11 display server's platform layer: the display,
 * windows and events (sdlgfx.h's pl_*), with Xlib.
 * MIT licence, see ../LICENSE.md.
 *
 * One thread: pl_main runs the protocol reader directly, and pl_wait
 * waits on the X connection. Events are translated as Icon's X11
 * backend translates them (keys by XLookupString), into the protocol's
 * conventions (sdlgfx.h; a motion's code is the button mask 1, 2, 4).
 */
#define _DEFAULT_SOURCE
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include "x11.h"

Display *xdpy;
Window xroot;
int xdepth;
static int xscreen;
static Visual *xvisual;
static int rshift, gshift, bshift;	/* the masks' positions, for 8 bits */
static unsigned long rmask, gmask, bmask;
static Atom wm_protocols, wm_delete, net_wm_name, utf8_string, net_wm_icon,
   net_wm_state, net_wm_state_max_h, net_wm_state_max_v, wm_state_atom;
static GC copygc;			/* plain copies to windows */

/* the position of a mask's top 8 bits, as a shift of an 8-bit value */
static int shift8(unsigned long m)
{
   int top = 0;
   for (int i = 0; i < (int)(8 * sizeof m); i++)
      if (m >> i & 1)
         top = i;
   return top - 7;
}

int x_open(void)
{
   static int tried;
   if (xdpy || tried)
      return xdpy != NULL;
   tried = 1;
   if (!(xdpy = XOpenDisplay(NULL)))
      return 0;
   xscreen = DefaultScreen(xdpy);
   xroot = RootWindow(xdpy, xscreen);
   xdepth = DefaultDepth(xdpy, xscreen);
   xvisual = DefaultVisual(xdpy, xscreen);
   if (xvisual->class != TrueColor || xdepth < 15) {
      fprintf(stderr, "gcon-graphics-x11: the display's default visual is not TrueColor\n");
      XCloseDisplay(xdpy);
      xdpy = NULL;
      return 0;
   }
   rmask = xvisual->red_mask; gmask = xvisual->green_mask; bmask = xvisual->blue_mask;
   rshift = shift8(rmask); gshift = shift8(gmask); bshift = shift8(bmask);
   wm_protocols = XInternAtom(xdpy, "WM_PROTOCOLS", False);
   wm_delete = XInternAtom(xdpy, "WM_DELETE_WINDOW", False);
   net_wm_name = XInternAtom(xdpy, "_NET_WM_NAME", False);
   utf8_string = XInternAtom(xdpy, "UTF8_STRING", False);
   net_wm_icon = XInternAtom(xdpy, "_NET_WM_ICON", False);
   net_wm_state = XInternAtom(xdpy, "_NET_WM_STATE", False);
   net_wm_state_max_h = XInternAtom(xdpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
   net_wm_state_max_v = XInternAtom(xdpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
   wm_state_atom = XInternAtom(xdpy, "WM_STATE", False);
   copygc = XCreateGC(xdpy, xroot, 0, NULL);
   XSetGraphicsExposures(xdpy, copygc, False);
   return 1;
}

static unsigned long place(unsigned int v8, int sh, unsigned long m)
{
   unsigned long v = v8 & 0xFF;
   return (sh >= 0 ? v << sh : v >> -sh) & m;
}

static unsigned int take(unsigned long p, int sh, unsigned long m)
{
   p &= m;
   unsigned long v = sh >= 0 ? p >> sh : p << -sh;
   return (unsigned int)(v & 0xFF);
}

unsigned long x_pixel(unsigned int rgb)
{
   return place(rgb >> 16, rshift, rmask) | place(rgb >> 8, gshift, gmask) | place(rgb, bshift, bmask);
}

unsigned int x_rgb(unsigned long p)
{
   return take(p, rshift, rmask) << 16 | take(p, gshift, gmask) << 8 | take(p, bshift, bmask);
}

const char *gfx_describe(void)
{
   static char s[64];
   snprintf(s, sizeof s, "X11 (Xlib, X%dR%d)", X_PROTOCOL, XlibSpecificationRelease);
   return s;
}

/* ---------- windows ---------- */

typedef struct Win {
   Window w;
   void *cv;			/* the canvas last shown in it */
   int x, y, ww, wh;		/* last known position and size */
   int mapped, state;
   int ov, o[4];		/* an overlay to remove: x, y, w, h */
   struct Win *next;
} Win;

static Win *wins;

static Win *find(Window w)
{
   for (Win *p = wins; p; p = p->next)
      if (p->w == w)
         return p;
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

static void inject(Win *w, const char *script);

static void sizehints(Win *w, int resizable, int x, int y)
{
   XSizeHints *h = XAllocSizeHints();
   if (!h)
      return;
   if (x != PL_NOPOS && y != PL_NOPOS) {
      h->flags |= USPosition | PPosition;
      h->x = x;
      h->y = y;
   }
   if (!resizable) {
      h->flags |= PMinSize | PMaxSize;
      h->min_width = h->max_width = w->ww;
      h->min_height = h->max_height = w->wh;
   }
   XSetWMNormalHints(xdpy, w->w, h);
   XFree(h);
}

void *pl_open(const char *title, int x, int y, int w, int h, int state,
   int resizable, int *scale)
{
   static int injected;
   XSetWindowAttributes a;
   Win *n;

   *scale = 1;
   if (!x_open())
      return NULL;
   if (!(n = calloc(1, sizeof *n)))
      return NULL;
   n->ww = w; n->wh = h;
   n->x = x == PL_NOPOS ? 0 : x;
   n->y = y == PL_NOPOS ? 0 : y;
   a.background_pixmap = None;		/* the canvas is copied in */
   a.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask
      | ExposureMask | StructureNotifyMask | PropertyChangeMask;
   a.bit_gravity = NorthWestGravity;
   n->w = XCreateWindow(xdpy, xroot, n->x, n->y, (unsigned)w, (unsigned)h, 0, xdepth,
      InputOutput, xvisual, CWBackPixmap | CWEventMask | CWBitGravity, &a);
   XSetWMProtocols(xdpy, n->w, &wm_delete, 1);
   sizehints(n, resizable, x, y);
   {
      XWMHints *wh = XAllocWMHints();
      if (wh) {
         wh->flags = InputHint | StateHint;
         wh->input = True;
         wh->initial_state = state == PL_ICONIC ? IconicState : NormalState;
         XSetWMHints(xdpy, n->w, wh);
         XFree(wh);
      }
   }
   {
      XClassHint ch = { "gcon", "Gcon" };
      XSetClassHint(xdpy, n->w, &ch);
   }
   n->state = state;
   n->next = wins;
   wins = n;
   pl_title(n, title);
   XMapWindow(xdpy, n->w);
   n->mapped = 1;
   if (state == PL_MAXIMAL)
      pl_state(n, PL_MAXIMAL);
   XFlush(xdpy);
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
   /* drop its queued events */
   for (Ev **p = &qhead; *p;) {
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
   XDestroyWindow(xdpy, w->w);
   XFlush(xdpy);
   free(w);
}

void pl_show(void *win)
{
   Win *w = win;
   if (w && !w->mapped) {
      XMapWindow(xdpy, w->w);
      w->mapped = 1;
   }
}

/* copy the canvas's x0, y0, x1, y1 to the window */
static void blit(Win *w, int x0, int y0, int x1, int y1)
{
   int cw, ch;
   Pixmap pm;
   if (!w->cv)
      return;
   pm = x_pixmap(w->cv, &cw, &ch);
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > cw) x1 = cw;
   if (y1 > ch) y1 = ch;
   if (x0 < x1 && y0 < y1)
      XCopyArea(xdpy, pm, w->w, copygc, x0, y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0), x0, y0);
}

void pl_present(void *win, void *canvas, int lw, int lh, int full)
{
   Win *w = win;
   int r[4];
   (void)lw; (void)lh;
   w->cv = canvas;
   if (x_take_dirty(canvas, r) && !full)
      blit(w, r[0], r[1], r[2], r[3]);
   if (full)
      blit(w, 0, 0, INT_MAX / 2, INT_MAX / 2);
   if (w->ov) {				/* the overlay goes */
      w->ov = 0;
      blit(w, w->o[0], w->o[1], w->o[0] + w->o[2], w->o[1] + w->o[3]);
   }
   XFlush(xdpy);
}

void pl_title(void *win, const char *s)
{
   Win *w = win;
   XStoreName(xdpy, w->w, s);
   XChangeProperty(xdpy, w->w, net_wm_name, utf8_string, 8, PropModeReplace,
      (const unsigned char *)s, (int)strlen(s));
}

void pl_geometry(void *win, int x, int y, int w, int h)
{
   Win *p = win;
   if (w > 0 && h > 0) {
      p->ww = w; p->wh = h;
      if (x != PL_NOPOS && y != PL_NOPOS)
         XMoveResizeWindow(xdpy, p->w, x, y, (unsigned)w, (unsigned)h);
      else
         XResizeWindow(xdpy, p->w, (unsigned)w, (unsigned)h);
   }
   else if (x != PL_NOPOS && y != PL_NOPOS)
      XMoveWindow(xdpy, p->w, x, y);
   XFlush(xdpy);
}

int pl_position(void *win, int *x, int *y)
{
   Win *w = win;
   Window child;
   XSync(xdpy, False);
   if (!XTranslateCoordinates(xdpy, w->w, xroot, 0, 0, x, y, &child))
      return 0;
   return 1;
}

/* _NET_WM_STATE: add (1) or remove (0) the maximized states */
static void maximize(Win *w, int on)
{
   XEvent e;
   memset(&e, 0, sizeof e);
   e.xclient.type = ClientMessage;
   e.xclient.window = w->w;
   e.xclient.message_type = net_wm_state;
   e.xclient.format = 32;
   e.xclient.data.l[0] = on ? 1 : 0;
   e.xclient.data.l[1] = (long)net_wm_state_max_h;
   e.xclient.data.l[2] = (long)net_wm_state_max_v;
   e.xclient.data.l[3] = 1;
   XSendEvent(xdpy, xroot, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

void pl_state(void *win, int state)
{
   Win *w = win;
   switch (state) {
   case PL_ICONIC:
      XIconifyWindow(xdpy, w->w, xscreen);
      break;
   case PL_MAXIMAL:
      maximize(w, 1);
      break;
   case PL_NORMAL:
      if (w->state == PL_MAXIMAL)
         maximize(w, 0);
      XMapRaised(xdpy, w->w);
      w->mapped = 1;
      break;
   default:
      break;
   }
   w->state = state;
   XFlush(xdpy);
}

void pl_resizable(void *win, int on)
{
   Win *w = win;
   sizehints(w, on, PL_NOPOS, PL_NOPOS);
   XFlush(xdpy);
}

void pl_raise(void *win) { if (win) { XRaiseWindow(xdpy, ((Win *)win)->w); XFlush(xdpy); } }
void pl_lower(void *win) { if (win) { XLowerWindow(xdpy, ((Win *)win)->w); XFlush(xdpy); } }

void pl_cursor(void *win, int cursor)
{
   static const struct { int plc; unsigned shape; } map[] = {
      { PLC_DEFAULT, XC_left_ptr }, { PLC_TEXT, XC_xterm }, { PLC_WAIT, XC_watch },
      { PLC_CROSSHAIR, XC_crosshair }, { PLC_NS, XC_sb_v_double_arrow },
      { PLC_EW, XC_sb_h_double_arrow }, { PLC_MOVE, XC_fleur }, { PLC_NO, XC_X_cursor },
      { PLC_POINTER, XC_hand2 }, { PLC_NW, XC_top_left_corner }, { PLC_N, XC_top_side },
      { PLC_NE, XC_top_right_corner }, { PLC_E, XC_right_side },
      { PLC_SE, XC_bottom_right_corner }, { PLC_S, XC_bottom_side },
      { PLC_SW, XC_bottom_left_corner }, { PLC_W, XC_left_side },
   };
   Win *w = win;
   unsigned shape = XC_left_ptr;
   for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
      if (map[i].plc == cursor)
         shape = map[i].shape;
   Cursor c = XCreateFontCursor(xdpy, shape);
   XDefineCursor(xdpy, w->w, c);
   XFreeCursor(xdpy, c);
   XFlush(xdpy);
}

void pl_warp(void *win, int x, int y)
{
   Win *w = win;
   XWarpPointer(xdpy, None, w->w, 0, 0, 0, 0, x, y);
   XFlush(xdpy);
}

void pl_beep(int volume)
{
   if (!x_open())
      return;
   if (volume > 100) volume = 100;
   if (volume < -100) volume = -100;
   XBell(xdpy, volume);
   XFlush(xdpy);
}

void pl_icon(void *win, const unsigned int *px, int w, int h)
{
   Win *p = win;
   size_t n = (size_t)w * h + 2;
   unsigned long *d = malloc(n * sizeof *d);
   if (!d)
      return;
   d[0] = (unsigned long)w;
   d[1] = (unsigned long)h;
   for (size_t i = 0; i < (size_t)w * h; i++)
      d[i + 2] = px[i];
   XChangeProperty(xdpy, p->w, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
      (const unsigned char *)d, (int)n);
   free(d);
   XFlush(xdpy);
}

void pl_overlay(void *win, int x, int y, int w, int h, unsigned int rgb, int scale)
{
   Win *p = win;
   (void)scale;
   XSetForeground(xdpy, copygc, x_pixel(rgb));
   XFillRectangle(xdpy, p->w, copygc, x, y, (unsigned)w, (unsigned)h);
   if (p->ov) {				/* the union with one not yet removed */
      int x1 = p->o[0] + p->o[2] > x + w ? p->o[0] + p->o[2] : x + w;
      int y1 = p->o[1] + p->o[3] > y + h ? p->o[1] + p->o[3] : y + h;
      if (p->o[0] < x) x = p->o[0];
      if (p->o[1] < y) y = p->o[1];
      w = x1 - x;
      h = y1 - y;
   }
   p->ov = 1;
   p->o[0] = x; p->o[1] = y; p->o[2] = w; p->o[3] = h;
   XFlush(xdpy);
}

void pl_display_size(void *win, int *w, int *h)
{
   (void)win;
   if (!x_open()) {
      *w = *h = 0;
      return;
   }
   *w = DisplayWidth(xdpy, xscreen);
   *h = DisplayHeight(xdpy, xscreen);
}

int pl_global_mouse(int *x, int *y)
{
   Window r, c;
   int wx, wy;
   unsigned m;
   if (!x_open())
      return 0;
   return XQueryPointer(xdpy, xroot, &r, &c, x, y, &wx, &wy, &m) ? 1 : 0;
}

/* ---------- events ---------- */

static long xmods(unsigned state)
{
   return (long)(state & (ShiftMask | ControlMask | Mod1Mask));
}

/* A key press as Icon's X11 backend reports it: the character
 * XLookupString gives (shift and control applied), else the keysym;
 * modifier keys alone report nothing. */
static void key(Win *w, XKeyEvent *ke)
{
   char buf[16];
   KeySym ks = NoSymbol;
   pl_event e;
   int n = XLookupString(ke, buf, sizeof buf, &ks, NULL);

   if (IsModifierKey(ks) || ks == XK_Mode_switch || ks == XK_Num_Lock)
      return;
   memset(&e, 0, sizeof e);
   e.win = w;
   if (n == 1) {
      e.kind = PL_KEYCHAR;
      e.code = (unsigned char)buf[0];
   }
   else if (ks != NoSymbol) {
      e.kind = PL_KEYSYM;
      e.code = (long)ks;
   }
   else
      return;
   e.x = ke->x;
   e.y = ke->y;
   e.mods = xmods(ke->state);
   e.ms = ke->time;
   enqueue(&e);
}

/* the window's state from WM_STATE, after a map or unmap */
static int wmstate(Win *w)
{
   Atom type;
   int fmt;
   unsigned long n, after;
   unsigned char *d = NULL;
   int st = PL_NORMAL;
   if (XGetWindowProperty(xdpy, w->w, wm_state_atom, 0, 2, False, wm_state_atom, &type, &fmt,
          &n, &after, &d) == Success && d && n >= 1 && fmt == 32)
      st = ((long *)d)[0] == IconicState ? PL_ICONIC : PL_NORMAL;
   if (d)
      XFree(d);
   return st;
}

static void translate(XEvent *xe)
{
   pl_event e;
   Win *w = find(xe->xany.window);

   if (!w)
      return;
   memset(&e, 0, sizeof e);
   e.win = w;
   switch (xe->type) {
   case KeyPress:
      key(w, &xe->xkey);
      return;
   case ButtonPress:
   case ButtonRelease:
      if (xe->xbutton.button < 1 || xe->xbutton.button > 3)
         return;			/* the wheel, as SDL's server ignores it */
      e.kind = xe->type == ButtonPress ? PL_PRESS : PL_RELEASE;
      e.code = (long)xe->xbutton.button;
      e.x = xe->xbutton.x;
      e.y = xe->xbutton.y;
      e.mods = xmods(xe->xbutton.state);
      e.ms = xe->xbutton.time;
      break;
   case MotionNotify: {
      unsigned s = xe->xmotion.state;
      e.kind = PL_MOTION;
      e.code = (s & Button1Mask ? 1 : 0) | (s & Button2Mask ? 2 : 0) | (s & Button3Mask ? 4 : 0);
      e.x = xe->xmotion.x;
      e.y = xe->xmotion.y;
      e.mods = xmods(s);
      e.ms = xe->xmotion.time;
      break;
   }
   case Expose:
      blit(w, xe->xexpose.x, xe->xexpose.y, xe->xexpose.x + xe->xexpose.width,
         xe->xexpose.y + xe->xexpose.height);
      if (xe->xexpose.count > 0)
         return;
      e.kind = PL_EXPOSE;
      break;
   case ConfigureNotify: {
      int x, y;
      Window child;
      if (xe->xconfigure.width != w->ww || xe->xconfigure.height != w->wh) {
         w->ww = xe->xconfigure.width;
         w->wh = xe->xconfigure.height;
         e.kind = PL_RESIZE;
         e.x = w->ww;
         e.y = w->wh;
         enqueue(&e);
      }
      if (XTranslateCoordinates(xdpy, w->w, xroot, 0, 0, &x, &y, &child)
          && (x != w->x || y != w->y)) {
         w->x = x;
         w->y = y;
         e.kind = PL_MOVE;
         e.x = x;
         e.y = y;
         enqueue(&e);
      }
      return;
   }
   case MapNotify:
   case UnmapNotify: {
      int st = xe->type == MapNotify ? PL_NORMAL : wmstate(w);
      if (xe->type == UnmapNotify && st != PL_ICONIC)
         return;
      e.kind = PL_STATE;
      e.code = st;
      break;
   }
   case ClientMessage:
      if (xe->xclient.message_type != wm_protocols || (Atom)xe->xclient.data.l[0] != wm_delete)
         return;
      e.kind = PL_CLOSE;
      break;
   default:
      return;
   }
   enqueue(&e);
}

static void drain(void)
{
   while (XPending(xdpy)) {
      XEvent xe;
      XNextEvent(xdpy, &xe);
      translate(&xe);
   }
}

int pl_poll(pl_event *e)
{
   if (xdpy)
      drain();
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
   struct pollfd p;
   if (qhead)
      return 1;
   if (!xdpy) {
      poll(NULL, 0, timeout_ms);
      return 0;
   }
   XFlush(xdpy);
   drain();
   if (qhead)
      return 1;
   p.fd = ConnectionNumber(xdpy);
   p.events = POLLIN;
   if (poll(&p, 1, timeout_ms) > 0)
      drain();
   return qhead != NULL;
}

/* ---------- scripted input, for tests ---------- */

/*
 * ICONSDL_TEST_EVENTS, as the SDL server reads it (../src/plat.c): when
 * the first window opens, its items are queued as if typed or clicked
 * in that window. Keys go through XLookupString as typed ones do.
 *
 *   key:NAME          NAME a keysym name ("a", "Return", "Left", "F1"),
 *                     optionally after shift+, ctrl+, alt+ or cmd+
 *   down:B:X:Y        mouse button B pressed at X, Y
 *   up:B:X:Y          and released
 *   move:B:X:Y        pointer moved with button B held (0: none)
 *   close             the window's close box
 */
static KeySym keysym_named(const char *name)
{
   static const struct { const char *sdl, *x; } alias[] = {
      { "Backspace", "BackSpace" }, { "Space", "space" }, { "Enter", "Return" },
      { "PageUp", "Prior" }, { "PageDown", "Next" },
   };
   KeySym ks = XStringToKeysym(name);
   if (ks != NoSymbol)
      return ks;
   for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
      if (!strcasecmp(name, alias[i].sdl))
         return XStringToKeysym(alias[i].x);
   return NoSymbol;
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
         char *name = t + 4;
         unsigned state = 0;
         XKeyEvent ke;
         KeySym ks;
         for (;;) {
            if (!strncmp(name, "shift+", 6)) { state |= ShiftMask; name += 6; }
            else if (!strncmp(name, "ctrl+", 5)) { state |= ControlMask; name += 5; }
            else if (!strncmp(name, "alt+", 4)) { state |= Mod1Mask; name += 4; }
            else if (!strncmp(name, "cmd+", 4)) { state |= Mod1Mask; name += 4; }
            else break;
         }
         if ((ks = keysym_named(name)) == NoSymbol)
            continue;
         memset(&ke, 0, sizeof ke);
         ke.type = KeyPress;
         ke.display = xdpy;
         ke.window = w->w;
         ke.root = xroot;
         ke.state = state;
         ke.keycode = XKeysymToKeycode(xdpy, ks);
         ke.same_screen = True;
         if (ke.keycode == 0)
            continue;
         key(w, &ke);
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
   if (xdpy) {
      XCloseDisplay(xdpy);
      xdpy = NULL;
   }
}
