/*
 * cocoa.h -- what the Cocoa display server's layers share (cplat.m
 * windows and events, ccanvas.m canvases, cfont.m fonts).
 * MIT licence, see ../LICENSE.md.
 *
 * The server is ../src/main.c (the protocol reader) over the interface
 * of ../src/sdlgfx.h. The reader runs on a thread of its own, as in the
 * SDL server, and AppKit has the main thread (pl_main): window requests
 * are carried out there, and events come back through a queue.
 *
 * A canvas is a Core Graphics bitmap context over pixels of its own, at
 * an integer scale (the display's backing scale for a window, 1 for a
 * hidden canvas), 0xFFRRGGBB in memory (premultiplied ARGB, little
 * endian), as the SDL server's: so its pixel operations (copy, read,
 * write, the X GC functions) are that server's. Drawing in copy mode
 * without a pattern is Core Graphics's own, antialiased, and text is
 * CoreText's; any other drawop, drawop=reverse and patterns are drawn
 * aliased into a coverage mask and combined pixel by pixel, as X does.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include "sdlgfx.h"

/* the canvas as an image (a copy), and whether it changed since the
 * last call (cleared); its scale */
CGImageRef c_image(void *cv, int *changed);
int c_changed(void *cv);

/* the font's CoreText font and metrics */
CTFontRef c_font(void *f);
