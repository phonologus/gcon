/*
 * Graphics: Icon's graphics facilities, as iconx's device-independent
 * layer (fwindow.r, rwindow.r, rcolor.r) and icon-sdl's SDL backend
 * (rsdlwin.ri, rsdlrsc.ri) have them, with the display in a separate
 * server (gcon-graphics) reached through its protocol (gfxproto.c). See
 * design/design.md, "Graphics".
 */
#ifndef GCON_VM_GFX_H
#define GCON_VM_GFX_H

#include "vm.h"

/* ---- The protocol (doc/internals/protocol.md) ---- */

enum {
	P_HELLO, P_CANVAS, P_FREE, P_RESIZE, P_GC, P_POINTS, P_LINES, P_FILLPOLY,
	P_SEGMENTS, P_RECTS, P_FILLRECTS, P_ARCS, P_ERASE, P_COPY, P_WRITE, P_READ,
	P_COLOR, P_WAIT, P_SHOW, P_FONT, P_TEXT, P_WIDTH, P_TITLE, P_GEOMETRY,
	P_STATE, P_MAP, P_RESIZABLE, P_POSITION, P_RAISE, P_LOWER, P_CURSOR,
	P_WARP, P_BEEP, P_DISPLAYSIZE, P_GLOBALMOUSE, P_OVERLAY,
	P_RESERVED36, P_RESERVED37, /* version 2's ENCODE and DECODE: image files are gcon-vm's */
	P_ICON,
};

#define NOPOS INT32_MIN

/* The platform's window states and events (gcon-graphics's sdlgfx.h). */
enum { PL_NORMAL, PL_ICONIC, PL_MAXIMAL, PL_HIDDEN };
enum {
	PL_KEYCHAR = 1, PL_KEYSYM, PL_PRESS, PL_RELEASE, PL_MOTION, PL_RESIZE,
	PL_MOVE, PL_EXPOSE, PL_STATE, PL_CLOSE,
};

bool gp_connect(void);
bool selfexe(char *self); /* gcon-vm's executable's path, PATH_MAX bytes */
void gp_close(void);
void gp_begin(int op);
void gp_u8(int v);
void gp_u32(uint32_t v);
void gp_i32(int32_t v);
void gp_f64(double d);
void gp_bytes(const void *p, size_t n);
void gp_str(const char *s, size_t n);
void gp_end(void);
void gp_flush(void);
void gp_reply(int op);
uint32_t gr_u32(void);
int32_t gr_i32(void);
int64_t gr_i64(void);
void gr_bytes(void *p, size_t n);

/* ---- Iconx's definitions (graphics.h, icon-sdl's sdlwin.h) ---- */

enum {
	DRAWOP_CLEAR, DRAWOP_AND, DRAWOP_ANDREVERSE, DRAWOP_COPY, DRAWOP_ANDINVERTED,
	DRAWOP_NOOP, DRAWOP_XOR, DRAWOP_OR, DRAWOP_NOR, DRAWOP_EQUIV, DRAWOP_INVERT,
	DRAWOP_ORREVERSE, DRAWOP_COPYINVERTED, DRAWOP_ORINVERTED, DRAWOP_NAND, DRAWOP_SET,
};
enum { LS_SOLID = 0, LS_DASHED = 1, LS_STRIPED = 2 };
enum { FS_SOLID = 0, FS_STIPPLE = 2, FS_OPAQUESTIPPLE = 3 };
#define ShiftMask (1L << 0)
#define ControlMask (1L << 2)
#define Mod1Mask (1L << 3)
enum { NormalState = 1, IconicState = 3, RootState = 4, MaximizedState = 5, HiddenState = 6 };
#define EQ_MOD_CONTROL (1L << 16)
#define EQ_MOD_META (1L << 17)
#define EQ_MOD_SHIFT (1L << 18)
enum { EQUEUELEN = 256, MAXCOLORNAME = 40, MAXFONTWORD = 40, DEFAULTFONTSIZE = 14 };

/* Mouse event codes (&lpress ...) and &resize. */
enum {
	MOUSELEFT = -1, MOUSEMID = -2, MOUSERIGHT = -3, MOUSELEFTUP = -4, MOUSEMIDUP = -5,
	MOUSERIGHTUP = -6, MOUSELEFTDRAG = -7, MOUSEMIDDRAG = -8, MOUSERIGHTDRAG = -9,
	RESIZED = -10,
};

typedef struct WFont {
	char *name;
	uint32_t id;
	int ascent, descent, fwidth, height;
	struct WFont *next;
} WFont;

/* A graphics context (iconx's wcontext). */
typedef struct WContext {
	int refcount, serial;
	uint32_t gc;        /* its gc in the server */
	bool sent;          /* the server's gc is current */
	int clipx, clipy, clipw, cliph;
	char *patternname;
	WFont *font;
	int dx, dy;
	int fillstyle, drawop;
	double gamma;
	int bits;           /* 8 reverse, 16 drawop=reverse */
	unsigned fg, bg;    /* device colours 0xRRGGBB */
	char fgname[6 + MAXCOLORNAME], bgname[6 + MAXCOLORNAME];
	int linestyle, linewidth, leading;
	int antialias;      /* blend by coverage (gcon's antialias attribute) */
	int patw, path;
	unsigned char *pat;
	struct WContext *next;
} WContext;

/* A canvas, and the window showing it (iconx's wstate). */
typedef struct WState {
	int refcount, serial;
	uint32_t id;        /* its canvas in the server; 0 until made */
	bool win;           /* it has a window (not hidden) */
	int pixheight, pixwidth;
	char *windowlabel, *iconlabel, *iconimage, *cursorname;
	int y, x;           /* the text cursor */
	int pointery, pointerx;
	int posy, posx;     /* desired position; -MaxInt: the system's choice */
	int height, width;
	int bits;           /* 1 closed, 2 cursor on, 128 resizable, 256 exposed,
	                     * 512 ceol, 1024 echo */
	uint64_t timestamp; /* the last event's time */
	char eventQueue[EQUEUELEN]; /* keystrokes, for backspace */
	int eQfront, eQback;
	List *listp;        /* the Icon event list */
	File *filep;        /* the window value it was opened as */
	int iconic;
	unsigned *initpx;       /* image=: drawn when the canvas is made */
	unsigned char *initop;
	int initw, inith;
	unsigned *iconpx;       /* iconimage=: 0xAARRGGBB, sent to the window */
	int iconw, iconh;
	struct WState *next;
} WState;

/* A window value: a canvas and a context (iconx's wbinding). */
typedef struct WBinding {
	WState *window;
	WContext *context;
} WBinding;

#define MaxInt INT32_MAX
#define ASCENT(w) ((w)->context->font->ascent)
#define DESCENT(w) ((w)->context->font->descent)
#define FHEIGHT(w) ((w)->context->font->height)
#define FWIDTH(w) ((w)->context->font->fwidth)
#define LEADING(w) ((w)->context->leading)
#define MAXDESCENDER(w) DESCENT(w)
#define ROWTOY(w, row) (((row) - 1) * LEADING(w) + ASCENT(w))
#define COLTOX(w, col) (((col) - 1) * FWIDTH(w))
#define YTOROW(w, y) (((y) > 0) ? ((y) / LEADING(w) + 1) : ((y) / LEADING(w)))
#define XTOCOL(w, x) (!FWIDTH(w) ? (x) : ((x) / FWIDTH(w)))
#define ISCLOSED(w) ((w)->window->bits & 1)
#define ISCURSORON(w) ((w)->window->bits & 2)
#define ISRESIZABLE(w) ((w)->window->bits & 128)
#define ISEXPOSED(w) ((w)->window->bits & 256)
#define ISCEOLON(w) ((w)->window->bits & 512)
#define ISECHOON(w) ((w)->window->bits & 1024)
#define ISREVERSE(w) ((w)->context->bits & 8)
#define ISXORREVERSE(w) ((w)->context->bits & 16)

/* ---- Hooks for the rest of gcon-vm ---- */

File *gfx_open(String name, Value *attrs, int n); /* open(s, "g", ...); NULL: fail */
void gfx_close(File *f);
void gfx_image(Buf *b, File *f);
void gfx_mark(void);
void gfx_exit(void);
void gfx_write(File *f, const char *s, size_t n); /* write() to a window */
bool gfx_read(File *f, int64_t k, String *out); /* read() (k < 0), reads(): with echo */
Value gfx_kwget(int k);
bool gfx_kwset(int k, Value x);
void graphics_init(void);

#endif
