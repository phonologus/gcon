/*
 * cplat.m -- the Cocoa display server's platform layer: windows and
 * events (sdlgfx.h's pl_*), with AppKit. MIT licence, see ../LICENSE.md.
 *
 * AppKit has the main thread: pl_main runs the protocol reader on a
 * thread of its own (as the SDL server does) and the application's run
 * loop here. Window requests run on the main thread (dispatch_sync);
 * events are translated there, as ../src/plat.c translates SDL's (X's
 * conventions: characters as XLookupString gives them, keysyms, X
 * modifier masks, a motion's button mask 1, 2, 4), into a queue the
 * reader waits on. A window shows its canvas as its layer's contents,
 * an image of the canvas made on each showing.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cocoa.h"

#define X_SHIFT   (1L << 0)
#define X_CONTROL (1L << 2)
#define X_MOD1    (1L << 3)

/* ---------- the event queue ---------- */

typedef struct Ev { pl_event e; struct Ev *next; } Ev;
static Ev *qhead, *qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;

static void enqueue(const pl_event *e)
{
   Ev *n = malloc(sizeof *n);
   if (!n)
      return;
   n->e = *e;
   n->next = NULL;
   pthread_mutex_lock(&qlock);
   if (qtail)
      qtail->next = n;
   else
      qhead = n;
   qtail = n;
   pthread_cond_signal(&qcond);
   pthread_mutex_unlock(&qlock);
}

int pl_poll(pl_event *e)
{
   Ev *h;
   pthread_mutex_lock(&qlock);
   if ((h = qhead) != NULL) {
      qhead = h->next;
      if (!qhead)
         qtail = NULL;
   }
   pthread_mutex_unlock(&qlock);
   if (!h)
      return 0;
   *e = h->e;
   free(h);
   return 1;
}

int pl_wait(int timeout_ms)
{
   int r;
   pthread_mutex_lock(&qlock);
   if (!qhead && timeout_ms != 0) {
      if (timeout_ms < 0)
         pthread_cond_wait(&qcond, &qlock);
      else {
         struct timespec t;
         clock_gettime(CLOCK_REALTIME, &t);
         t.tv_sec += timeout_ms / 1000;
         t.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
         if (t.tv_nsec >= 1000000000) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000;
         }
         pthread_cond_timedwait(&qcond, &qlock, &t);
      }
   }
   r = qhead != NULL;
   pthread_mutex_unlock(&qlock);
   return r;
}

static void onmain(void (^b)(void))
{
   if ([NSThread isMainThread])
      b();
   else
      dispatch_sync(dispatch_get_main_queue(), b);
}

/* ---------- windows ---------- */

@class GView;

@interface GWin : NSObject <NSWindowDelegate>
{
@public
   NSWindow *win;
   GView *view;
   CALayer *overlay;
   void *cv;			/* the canvas last shown */
   int scale, shown, state, closing;
}
@end

@interface GView : NSView
{
@public
   __weak GWin *gw;
}
@end

static NSMutableArray *wins;		/* the open GWins */
static int meta_is_command = -1;	/* &meta: Option (default) or Command */

static double primaryheight(void)
{
   NSArray *s = [NSScreen screens];
   return s.count ? ((NSScreen *)s[0]).frame.size.height : 0;
}

static long xmods(NSEventModifierFlags m)
{
   long x = 0;
   if (meta_is_command < 0) {
      const char *s = getenv("ICONSDL_META");
      meta_is_command = s && !strcasecmp(s, "command");
   }
   if (m & NSEventModifierFlagShift) x |= X_SHIFT;
   if (m & NSEventModifierFlagControl) x |= X_CONTROL;
   if (m & (meta_is_command ? NSEventModifierFlagCommand : NSEventModifierFlagOption)) x |= X_MOD1;
   return x;
}

static unsigned long ms(NSEvent *e) { return (unsigned long)(e.timestamp * 1000.0); }

static void post(GWin *w, int kind, long code, int x, int y, long mods, unsigned long t)
{
   pl_event e;
   memset(&e, 0, sizeof e);
   e.win = (__bridge void *)w;
   e.kind = kind;
   e.code = code;
   e.x = x;
   e.y = y;
   e.mods = mods;
   e.ms = t;
   enqueue(&e);
}

/* X keysyms for keys that give no character (X11/keysymdef.h) */
static long keysym(unichar c)
{
   switch (c) {
   case NSLeftArrowFunctionKey: return 0xFF51;
   case NSUpArrowFunctionKey: return 0xFF52;
   case NSRightArrowFunctionKey: return 0xFF53;
   case NSDownArrowFunctionKey: return 0xFF54;
   case NSPageUpFunctionKey: return 0xFF55;
   case NSPageDownFunctionKey: return 0xFF56;
   case NSEndFunctionKey: return 0xFF57;
   case NSHomeFunctionKey: return 0xFF50;
   case NSInsertFunctionKey: return 0xFF63;
   case NSPrintScreenFunctionKey: return 0xFF61;
   case NSPauseFunctionKey: return 0xFF13;
   case NSScrollLockFunctionKey: return 0xFF14;
   case NSHelpFunctionKey: return 0xFF6A;
   case NSMenuFunctionKey: return 0xFF67;
   default:
      if (c >= NSF1FunctionKey && c <= NSF12FunctionKey)
         return 0xFFBE + (c - NSF1FunctionKey);
      return 0;
   }
}

/* control applied to a character, as XLookupString applies it */
static long control(long ch)
{
   if (ch >= '@' && ch <= '_') return ch - '@';
   if (ch >= 'a' && ch <= 'z') return ch - ('a' - 1);
   if (ch == ' ') return 0;
   return ch;
}

@implementation GView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)isOpaque { return YES; }

- (void)where:(NSEvent *)e x:(int *)x y:(int *)y
{
   NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
   *x = (int)floor(p.x);
   *y = (int)floor(self.bounds.size.height - p.y);
}

/* a key as X's XLookupString reports it: a character (shift and
 * control applied; Option, as &meta, not), else a keysym; modifier
 * keys alone report nothing */
- (void)keyDown:(NSEvent *)e
{
   NSString *s = e.charactersIgnoringModifiers;
   NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
   long ch = -1, ks = 0;
   unichar c;
   if (!gw || s.length == 0)
      return;
   c = [s characterAtIndex:0];
   switch (c) {
   case NSCarriageReturnCharacter: case NSEnterCharacter: ch = '\r'; break;
   case NSTabCharacter: case NSBackTabCharacter: ch = '\t'; break;
   case NSDeleteCharacter: ch = '\b'; break;		/* the Delete key: X's BackSpace */
   case 0x1B: ch = 033; break;
   case NSDeleteFunctionKey: ch = 0177; break;
   default:
      if ((ks = keysym(c)) != 0)
         break;
      if (c < 32 || c >= 256 || c == 127)
         return;
      ch = c;
      if (e.modifierFlags & NSEventModifierFlagControl)
         ch = control(ch);
   }
   post(gw, ks ? PL_KEYSYM : PL_KEYCHAR, ks ? ks : ch, (int)floor(p.x),
      (int)floor(self.bounds.size.height - p.y), xmods(e.modifierFlags), ms(e));
}

- (void)button:(NSEvent *)e code:(int)b kind:(int)kind
{
   int x, y;
   if (!gw)
      return;
   [self where:e x:&x y:&y];
   post(gw, kind, b, x, y, xmods(e.modifierFlags), ms(e));
}

- (void)mouseDown:(NSEvent *)e { [self button:e code:1 kind:PL_PRESS]; }
- (void)mouseUp:(NSEvent *)e { [self button:e code:1 kind:PL_RELEASE]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:e code:3 kind:PL_PRESS]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:e code:3 kind:PL_RELEASE]; }
- (void)otherMouseDown:(NSEvent *)e { if (e.buttonNumber == 2) [self button:e code:2 kind:PL_PRESS]; }
- (void)otherMouseUp:(NSEvent *)e { if (e.buttonNumber == 2) [self button:e code:2 kind:PL_RELEASE]; }

- (void)motion:(NSEvent *)e
{
   NSUInteger b = [NSEvent pressedMouseButtons];
   int x, y;
   if (!gw)
      return;
   [self where:e x:&x y:&y];
   post(gw, PL_MOTION, (b & 1 ? 1 : 0) | (b & 4 ? 2 : 0) | (b & 2 ? 4 : 0), x, y,
      xmods(e.modifierFlags), ms(e));
}

- (void)mouseMoved:(NSEvent *)e { [self motion:e]; }
- (void)mouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self motion:e]; }
@end

@implementation GWin
- (BOOL)windowShouldClose:(NSWindow *)w
{
   (void)w;
   post(self, PL_CLOSE, 0, 0, 0, 0, 0);
   return NO;				/* the program decides */
}

- (void)windowDidResize:(NSNotification *)n
{
   NSSize s = view.bounds.size;
   (void)n;
   post(self, PL_RESIZE, 0, (int)lround(s.width), (int)lround(s.height), 0, 0);
   if ([win isZoomed] != (state == PL_MAXIMAL)) {
      state = [win isZoomed] ? PL_MAXIMAL : PL_NORMAL;
      post(self, PL_STATE, state, 0, 0, 0, 0);
   }
}

- (void)windowDidMove:(NSNotification *)n
{
   NSRect c = [win contentRectForFrameRect:win.frame];
   (void)n;
   post(self, PL_MOVE, 0, (int)lround(c.origin.x),
      (int)lround(primaryheight() - c.origin.y - c.size.height), 0, 0);
}

- (void)windowDidMiniaturize:(NSNotification *)n
{
   (void)n;
   state = PL_ICONIC;
   post(self, PL_STATE, PL_ICONIC, 0, 0, 0, 0);
}

- (void)windowDidDeminiaturize:(NSNotification *)n
{
   (void)n;
   state = [win isZoomed] ? PL_MAXIMAL : PL_NORMAL;
   post(self, PL_STATE, state, 0, 0, 0, 0);
   post(self, PL_EXPOSE, 0, 0, 0, 0, 0);
}
@end

/* Command-Q: as closing the key (else the first) window */
@interface GApp : NSObject <NSApplicationDelegate>
@end

@implementation GApp
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a
{
   GWin *w = nil;
   (void)a;
   for (GWin *x in wins)
      if (x->win.isKeyWindow)
         w = x;
   if (!w && wins.count)
      w = wins[0];
   if (w)
      post(w, PL_CLOSE, 0, 0, 0, 0, 0);
   return NSTerminateCancel;
}
@end

/* the first window shown makes this a regular application: a Dock icon,
 * a menu (Quit), the focus */
static void becomeapp(void)
{
   static int done;
   NSMenu *bar, *menu;
   NSMenuItem *item;
   if (done)
      return;
   done = 1;
   [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
   bar = [[NSMenu alloc] init];
   item = [[NSMenuItem alloc] init];
   [bar addItem:item];
   menu = [[NSMenu alloc] init];
   [menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
   item.submenu = menu;
   NSApp.mainMenu = bar;
   [NSApp activateIgnoringOtherApps:YES];
}

static NSString *title(const char *s)
{
   NSString *t = [NSString stringWithUTF8String:s];
   if (!t)
      t = [NSString stringWithCString:s encoding:NSISOLatin1StringEncoding];
   return t ? t : @"";
}

static void inject(GWin *w, const char *script);

void *pl_open(const char *ttl, int x, int y, int w, int h, int state, int resizable, int *scale)
{
   __block GWin *gw = nil;
   NSString *t = title(ttl);
   static int injected;

   onmain(^{
      NSRect cr = NSMakeRect(0, 0, w, h);
      NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
         | NSWindowStyleMaskMiniaturizable | (resizable ? NSWindowStyleMaskResizable : 0);
      const char *sc = getenv("ICONSDL_SCALE");
      gw = [[GWin alloc] init];
      gw->win = [[NSWindow alloc] initWithContentRect:cr styleMask:style
         backing:NSBackingStoreBuffered defer:NO];
      gw->win.releasedWhenClosed = NO;
      gw->win.title = t;
      gw->win.acceptsMouseMovedEvents = YES;
      gw->win.delegate = gw;
      gw->view = [[GView alloc] initWithFrame:cr];
      gw->view->gw = gw;
      /* layer-hosting: the layer is ours, its contents the canvas */
      gw->view.layer = [CALayer layer];
      gw->view.wantsLayer = YES;
      gw->view.layer.contentsGravity = kCAGravityTopLeft;
      gw->view.layer.masksToBounds = YES;
      gw->view.layer.backgroundColor = CGColorGetConstantColor(kCGColorWhite);
      gw->win.contentView = gw->view;
      [gw->win makeFirstResponder:gw->view];
      if (x != PL_NOPOS && y != PL_NOPOS) {
         NSRect c = NSMakeRect(x, primaryheight() - y - h, w, h);
         [gw->win setFrame:[gw->win frameRectForContentRect:c] display:NO];
      }
      else
         [gw->win center];
      gw->scale = sc && atoi(sc) > 0 ? atoi(sc) : (int)lround(gw->win.backingScaleFactor);
      if (gw->scale < 1)
         gw->scale = 1;
      gw->view.layer.contentsScale = gw->scale;
      gw->state = state;
      if (!wins)
         wins = [NSMutableArray array];
      [wins addObject:gw];
   });
   *scale = gw->scale;
   if (!injected) {
      const char *s = getenv("ICONSDL_TEST_EVENTS");
      injected = 1;
      if (s && *s)
         inject(gw, s);
   }
   return (__bridge void *)gw;
}

void pl_close(void *win)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      w->closing = 1;
      w->win.delegate = nil;
      w->view->gw = nil;
      [w->win orderOut:nil];
      [w->win close];
      [wins removeObject:w];
   });
   /* drop its queued events */
   pthread_mutex_lock(&qlock);
   for (Ev **p = &qhead; *p;) {
      if ((*p)->e.win == win) {
         Ev *d = *p;
         *p = d->next;
         free(d);
      } else
         p = &(*p)->next;
   }
   qtail = NULL;
   for (Ev *e = qhead; e; e = e->next)
      qtail = e;
   pthread_mutex_unlock(&qlock);
}

static void show(GWin *w)
{
   becomeapp();
   [w->win makeKeyAndOrderFront:nil];
   if (!w->shown) {
      w->shown = 1;
      post(w, PL_EXPOSE, 0, 0, 0, 0, 0);
   }
}

void pl_show(void *win)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{ show(w); });
}

void pl_present(void *win, void *canvas, int lw, int lh, int full)
{
   GWin *w = (__bridge GWin *)win;
   CGImageRef img;
   int changed;
   (void)lw; (void)lh;
   if (!full && w->cv == canvas && !c_changed(canvas) && !w->overlay)
      return;
   w->cv = canvas;
   img = c_image(canvas, &changed);
   dispatch_async(dispatch_get_main_queue(), ^{
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      w->view.layer.contents = (__bridge id)img;
      if (w->overlay) {
         [w->overlay removeFromSuperlayer];
         w->overlay = nil;
      }
      [CATransaction commit];
      CGImageRelease(img);
   });
}

void pl_title(void *win, const char *s)
{
   GWin *w = (__bridge GWin *)win;
   NSString *t = title(s);
   onmain(^{ w->win.title = t; });
}

void pl_geometry(void *win, int x, int y, int wd, int ht)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      NSRect c = [w->win contentRectForFrameRect:w->win.frame];
      double top = primaryheight() - (c.origin.y + c.size.height);	/* Icon's y of the top */
      if (wd > 0 && ht > 0)
         c.size = NSMakeSize(wd, ht);
      if (x != PL_NOPOS && y != PL_NOPOS) {
         c.origin.x = x;
         top = y;
      }
      c.origin.y = primaryheight() - top - c.size.height;
      [w->win setFrame:[w->win frameRectForContentRect:c] display:YES];
   });
}

int pl_position(void *win, int *x, int *y)
{
   GWin *w = (__bridge GWin *)win;
   __block NSRect c;
   onmain(^{ c = [w->win contentRectForFrameRect:w->win.frame]; });
   *x = (int)lround(c.origin.x);
   *y = (int)lround(primaryheight() - c.origin.y - c.size.height);
   return 1;
}

void pl_state(void *win, int state)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      switch (state) {
      case PL_ICONIC:
         show(w);
         [w->win miniaturize:nil];
         break;
      case PL_MAXIMAL:
         if (w->win.miniaturized)
            [w->win deminiaturize:nil];
         show(w);
         if (!w->win.zoomed)
            [w->win zoom:nil];
         break;
      case PL_NORMAL:
         if (w->win.miniaturized)
            [w->win deminiaturize:nil];
         if (w->win.zoomed)
            [w->win zoom:nil];
         show(w);
         break;
      default:
         break;
      }
      w->state = state;
   });
}

void pl_resizable(void *win, int on)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      if (on)
         w->win.styleMask |= NSWindowStyleMaskResizable;
      else
         w->win.styleMask &= ~NSWindowStyleMaskResizable;
   });
}

void pl_raise(void *win)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{ [w->win orderFront:nil]; });
}

void pl_lower(void *win)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{ [w->win orderBack:nil]; });
}

void pl_cursor(void *win, int cursor)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      NSCursor *c;
      switch (cursor) {
      case PLC_TEXT: c = [NSCursor IBeamCursor]; break;
      case PLC_CROSSHAIR: c = [NSCursor crosshairCursor]; break;
      case PLC_NS: case PLC_N: case PLC_S: c = [NSCursor resizeUpDownCursor]; break;
      case PLC_EW: case PLC_E: case PLC_W: c = [NSCursor resizeLeftRightCursor]; break;
      case PLC_MOVE: c = [NSCursor openHandCursor]; break;
      case PLC_NO: c = [NSCursor operationNotAllowedCursor]; break;
      case PLC_POINTER: c = [NSCursor pointingHandCursor]; break;
      default: c = [NSCursor arrowCursor]; break;
      }
      [w->view discardCursorRects];
      [w->view addCursorRect:w->view.bounds cursor:c];
      [w->win invalidateCursorRectsForView:w->view];
      if (w->win.isKeyWindow)
         [c set];
   });
}

void pl_warp(void *win, int x, int y)
{
   GWin *w = (__bridge GWin *)win;
   onmain(^{
      NSRect c = [w->win contentRectForFrameRect:w->win.frame];
      CGPoint p = CGPointMake(c.origin.x + x, primaryheight() - (c.origin.y + c.size.height) + y);
      CGWarpMouseCursorPosition(p);
      CGAssociateMouseAndMouseCursorPosition(true);
   });
}

void pl_beep(int volume)
{
   (void)volume;
   onmain(^{ NSBeep(); });
}

void pl_icon(void *win, const unsigned int *px, int iw, int ih)
{
   CGContextRef k;
   CGImageRef img;
   CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
   unsigned int *copy = malloc((size_t)iw * ih * 4 + 4);
   (void)win;
   if (!copy) {
      CGColorSpaceRelease(cs);
      return;
   }
   memcpy(copy, px, (size_t)iw * ih * 4);
   k = CGBitmapContextCreate(copy, (size_t)iw, (size_t)ih, 8, (size_t)iw * 4, cs,
      (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
   CGColorSpaceRelease(cs);
   if (!k) {
      free(copy);
      return;
   }
   img = CGBitmapContextCreateImage(k);
   CGContextRelease(k);
   free(copy);
   onmain(^{
      NSImage *i = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(iw, ih)];
      NSApp.applicationIconImage = i;
   });
   CGImageRelease(img);
}

void pl_overlay(void *win, int x, int y, int wd, int ht, unsigned int rgb, int scale)
{
   GWin *w = (__bridge GWin *)win;
   (void)scale;
   dispatch_async(dispatch_get_main_queue(), ^{
      CGFloat r = (rgb >> 16 & 255) / 255.0, g = (rgb >> 8 & 255) / 255.0, b = (rgb & 255) / 255.0;
      CGColorRef col = CGColorCreateSRGB(r, g, b, 1);
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      if (!w->overlay) {
         w->overlay = [CALayer layer];
         [w->view.layer addSublayer:w->overlay];
      }
      w->overlay.backgroundColor = col;
      w->overlay.frame = CGRectMake(x, w->view.bounds.size.height - y - ht, wd, ht);
      [CATransaction commit];
      CGColorRelease(col);
   });
}

void pl_display_size(void *win, int *w, int *h)
{
   __block NSSize s = NSZeroSize;
   (void)win;
   onmain(^{
      NSScreen *m = [NSScreen mainScreen];
      if (m)
         s = m.frame.size;
   });
   *w = (int)lround(s.width);
   *h = (int)lround(s.height);
}

int pl_global_mouse(int *x, int *y)
{
   __block NSPoint p;
   onmain(^{ p = [NSEvent mouseLocation]; });
   *x = (int)floor(p.x);
   *y = (int)floor(primaryheight() - p.y);
   return 1;
}

/* ---------- scripted input, for tests ---------- */

/*
 * ICONSDL_TEST_EVENTS, as the SDL server reads it (../src/plat.c): when
 * the first window opens, its items are queued as if typed or clicked
 * in that window, translated as typed keys are (on a US layout).
 *
 *   key:NAME          a key: a character ("a", "1"), or Return, Tab,
 *                     BackSpace (Backspace), Escape, Delete, Space,
 *                     Left, Up, Right, Down, Home, End, PageUp,
 *                     PageDown, Insert, F1 ... F12; optionally after
 *                     shift+, ctrl+, alt+ or cmd+
 *   down:B:X:Y        mouse button B pressed at X, Y
 *   up:B:X:Y          and released
 *   move:B:X:Y        pointer moved with button B held (0: none)
 *   close             the window's close box
 */
static void injectkey(GWin *w, char *name)
{
   static const struct { const char *name; long ch, ks; } keys[] = {
      { "Return", '\r', 0 }, { "Tab", '\t', 0 }, { "BackSpace", '\b', 0 },
      { "Backspace", '\b', 0 }, { "Escape", 033, 0 }, { "Delete", 0177, 0 },
      { "Space", ' ', 0 }, { "Left", 0, 0xFF51 }, { "Up", 0, 0xFF52 },
      { "Right", 0, 0xFF53 }, { "Down", 0, 0xFF54 }, { "Home", 0, 0xFF50 },
      { "End", 0, 0xFF57 }, { "PageUp", 0, 0xFF55 }, { "PageDown", 0, 0xFF56 },
      { "Insert", 0, 0xFF63 },
   };
   static const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";
   static const char *plain   = "`1234567890-=[]\\;',./";
   long m = 0, ch = -1, ks = 0;
   int shift = 0, ctrl = 0, f;
   for (;;) {
      if (!strncmp(name, "shift+", 6)) { shift = 1; m |= X_SHIFT; name += 6; }
      else if (!strncmp(name, "ctrl+", 5)) { ctrl = 1; m |= X_CONTROL; name += 5; }
      else if (!strncmp(name, "alt+", 4)) { if (meta_is_command != 1) m |= X_MOD1; name += 4; }
      else if (!strncmp(name, "cmd+", 4)) { if (meta_is_command == 1) m |= X_MOD1; name += 4; }
      else break;
   }
   if (name[0] && !name[1]) {
      ch = (unsigned char)name[0];
      if (shift) {
         const char *q;
         if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
         else if ((q = strchr(plain, (int)ch)) != NULL)
            ch = (unsigned char)shifted[q - plain];
      }
      if (ctrl)
         ch = control(ch);
   }
   else if (sscanf(name, "F%d", &f) == 1 && f >= 1 && f <= 12)
      ks = 0xFFBE + f - 1;
   else {
      size_t i;
      for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
         if (!strcmp(name, keys[i].name))
            break;
      if (i == sizeof keys / sizeof keys[0])
         return;
      ch = keys[i].ch;
      ks = keys[i].ks;
   }
   post(w, ks ? PL_KEYSYM : PL_KEYCHAR, ks ? ks : ch, 0, 0, m, 0);
}

static void inject(GWin *w, const char *script)
{
   char *s = strdup(script), *save = NULL;
   if (!s)
      return;
   if (meta_is_command < 0)
      xmods(0);
   for (char *t = strtok_r(s, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
      int b = 0, x = 0, y = 0;
      if (!strncmp(t, "key:", 4))
         injectkey(w, t + 4);
      else if (sscanf(t, "down:%d:%d:%d", &b, &x, &y) == 3 || sscanf(t, "up:%d:%d:%d", &b, &x, &y) == 3)
         post(w, t[0] == 'd' ? PL_PRESS : PL_RELEASE, b == 1 ? 1 : b == 2 ? 2 : 3, x, y, 0, 0);
      else if (sscanf(t, "move:%d:%d:%d", &b, &x, &y) == 3)
         post(w, PL_MOTION, b == 0 ? 0 : b == 1 ? 1 : b == 2 ? 2 : 4, x, y, 0, 0);
      else if (!strcmp(t, "close"))
         post(w, PL_CLOSE, 0, 0, 0, 0, 0);
   }
   free(s);
}

/* ---------- the threads ---------- */

typedef struct { int (*f)(int, char **); int argc; char **argv; } Start;

static void *reader(void *p)
{
   Start *s = p;
   int r;
   @autoreleasepool {
      r = s->f(s->argc, s->argv);
   }
   exit(r);
   return NULL;
}

int pl_main(int (*f)(int, char **), int argc, char **argv)
{
   static Start s;
   pthread_t t;
   @autoreleasepool {
      static GApp *app;
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      app = [[GApp alloc] init];
      NSApp.delegate = app;
      [NSApp finishLaunching];
      s.f = f;
      s.argc = argc;
      s.argv = argv;
      if (pthread_create(&t, NULL, reader, &s) != 0) {
         fprintf(stderr, "gcon-graphics-cocoa: cannot start the reader\n");
         return 2;
      }
      [NSApp run];
   }
   return 0;
}

void pl_shutdown(void)
{
}

const char *gfx_describe(void)
{
   static char s[64];
   NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
   snprintf(s, sizeof s, "Cocoa (macOS %ld.%ld)", (long)v.majorVersion, (long)v.minorVersion);
   return s;
}

/* whether there is a display: a screen (none in a session without one) */
int pl_display(void)
{
   __block int n = 0;
   onmain(^{ n = (int)[NSScreen screens].count; });
   return n > 0;
}
