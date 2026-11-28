/*
 * x11.h -- what the X11 display server's three layers share
 * (xplat.c windows and events, xcanvas.c canvases, xfont.c fonts).
 * MIT licence, see ../LICENSE.md.
 *
 * The server is ../src/main.c (the protocol reader) over the interface
 * of ../src/sdlgfx.h, carried out with Xlib alone. A canvas is a Pixmap
 * of the default visual (TrueColor), at scale 1; a window shows its
 * canvas by copying it.
 */
#include <X11/Xlib.h>
#include "sdlgfx.h"

extern Display *xdpy;		/* NULL until x_open succeeds */
extern Window xroot;
extern int xdepth;

/* x_open opens the display (once); 0 if there is none. */
int x_open(void);

/* device pixels and 0xRRGGBB, through the visual's masks */
unsigned long x_pixel(unsigned int rgb);
unsigned int x_rgb(unsigned long pixel);

/* the canvas's pixmap and size, and what has changed since the last
 * call (x0, y0, x1, y1); 0 if nothing */
Pixmap x_pixmap(void *cv, int *w, int *h);
int x_take_dirty(void *cv, int r[4]);

/* the font's X structure */
XFontStruct *x_fontstruct(void *f);
