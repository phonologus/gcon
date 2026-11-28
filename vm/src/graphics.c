/*
 * Icon's graphics facilities (gfx.h): iconx's device-independent layer
 * (fwindow.r, rwindow.r, rcolor.r; public domain) and icon-sdl's SDL
 * backend (rsdlwin.ri, rsdlrsc.ri; MIT, Sean Jensen), with the display
 * reached through gcon-graphics's protocol (gfxproto.c) where icon-sdl
 * called its canvas and platform layers directly.
 */
#include "gfx.h"
#include "gimg.h"
#include "imgcodec.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define GENFN(f) static Gen *f(Builtin *b, Value *a, int n)

enum { Succeeded, Failed, Error };
#define Pi 3.14159265358979323846

static WState *wstates;
static WContext *wcntxts;
static WFont *fonts;
static int canvas_serial, context_serial;
static uint32_t lastcanvas, lastgc, lastfont;
static int nwins;            /* windows open (not hidden canvases) */
static uint64_t lastshow;    /* when windows were last shown */
static int errnum;           /* the error a wattrib Error raises */

/* The keywords. */
static Value kwindow = {MKH(KNull, 0, 0), {0}}, lastEventWin = {MKH(KNull, 0, 0), {0}};
static int64_t amperX, amperY, amperRow, amperCol, amperInterval;
static bool xmod_control, xmod_meta, xmod_shift;

static uint64_t millisec(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static char *salloc(const char *s) { return strdup(s ? s : ""); }

/* cstrval: a C string as an Icon string, copied into the heap (the
 * buffers here change or go). */
static Value cstrval(const char *s) { return strval(strcopy(s, (int64_t)strlen(s))); }

/* ---- string-integer tables (rwindow.r's stringint) ---- */

typedef struct { const char *s; int i; } StrInt;

static int si_s2i(const StrInt *t, int n, const char *s) {
	for (int k = 0; k < n; k++)
		if (!strcmp(t[k].s, s))
			return t[k].i;
	return -1;
}

static const char *si_i2s(const StrInt *t, int n, int i) {
	for (int k = 0; k < n; k++)
		if (t[k].i == i)
			return t[k].s;
	return NULL;
}

static const StrInt drawops[] = {
	{"and", DRAWOP_AND}, {"andInverted", DRAWOP_ANDINVERTED},
	{"andReverse", DRAWOP_ANDREVERSE}, {"clear", DRAWOP_CLEAR}, {"copy", DRAWOP_COPY},
	{"copyInverted", DRAWOP_COPYINVERTED}, {"equiv", DRAWOP_EQUIV},
	{"invert", DRAWOP_INVERT}, {"nand", DRAWOP_NAND}, {"noop", DRAWOP_NOOP},
	{"nor", DRAWOP_NOR}, {"or", DRAWOP_OR}, {"orInverted", DRAWOP_ORINVERTED},
	{"orReverse", DRAWOP_ORREVERSE}, {"set", DRAWOP_SET}, {"xor", DRAWOP_XOR},
};
#define NDRAWOPS (int)(sizeof drawops / sizeof drawops[0])

/* ---- Colours (rcolor.r; rsdlrsc.ri's device colours) ---- */

typedef struct { char name[8], ish[12]; short hue; char lgt, sat; } ColrName;
typedef struct { char word[10]; char val; } ColrMod;

static const ColrName colortable[] = {
	{"black", "blackish", 0, 0, 0}, {"blue", "bluish", 240, 50, 100},
	{"brown", "brownish", 30, 25, 100}, {"cyan", "cyanish", 180, 50, 100},
	{"gray", "grayish", 0, 50, 0}, {"green", "greenish", 120, 50, 100},
	{"grey", "greyish", 0, 50, 0}, {"magenta", "magentaish", 300, 50, 100},
	{"orange", "orangish", 15, 50, 100}, {"pink", "pinkish", 345, 75, 100},
	{"purple", "purplish", 270, 50, 100}, {"red", "reddish", 0, 50, 100},
	{"violet", "violetish", 270, 75, 100}, {"white", "whitish", 0, 100, 0},
	{"yellow", "yellowish", 60, 50, 100},
};
static const ColrMod lighttable[] = {
	{"dark", 0}, {"deep", 0}, {"light", 100}, {"medium", 50}, {"pale", 100},
};
static const ColrMod sattable[] = {
	{"moderate", 50}, {"strong", 75}, {"vivid", 100}, {"weak", 25},
};
#define COUNT(t) (int)(sizeof t / sizeof t[0])

static const ColrMod *findmod(const ColrMod *t, int n, const char *w) {
	for (int i = 0; i < n; i++)
		if (!strcmp(t[i].word, w))
			return &t[i];
	return NULL;
}

static const ColrName *findname(const char *w, bool ish) {
	for (int i = 0; i < COUNT(colortable); i++)
		if (!strcmp(ish ? colortable[i].ish : colortable[i].name, w))
			return &colortable[i];
	return NULL;
}

static double rgbval(double n1, double n2, double hue) {
	if (hue > 360)
		hue -= 360;
	else if (hue < 0)
		hue += 360;
	if (hue < 60)
		return n1 + (n2 - n1) * hue / 60.0;
	else if (hue < 180)
		return n2;
	else if (hue < 240)
		return n1 + (n2 - n1) * (240 - hue) / 60.0;
	return n1;
}

/* colorphrase parses an Icon colour phrase (rcolor.r's, verbatim in
 * its arithmetic). */
static int colorphrase(const char *in, long *r, long *g, long *b) {
	int len, very;
	char c, *p, *buf, *ebuf, cbuffer[MAXCOLORNAME];
	float lgt, sat, blend, bl2, m1, m2;
	float h1, l1, s1, h2, l2, s2, r2, g2, b2;
	const ColrMod *m;
	const ColrName *cn;

	lgt = -1.0;
	sat = 1.0;
	len = (int)strlen(in);
	while (len > 0 && isspace((unsigned char)in[len - 1]))
		len--;
	if (len >= (int)sizeof cbuffer)
		return 0;
	for (p = cbuffer; (c = *in) != 0; p++, in++) {
		if (isupper((unsigned char)c))
			*p = (char)tolower((unsigned char)c);
		else if (c == ' ' || c == '-')
			*p = '\0';
		else
			*p = c;
	}
	*p = '\0';
	buf = cbuffer;
	ebuf = buf + len;
	if (strcmp(buf, "very") == 0) {
		very = 1;
		buf += strlen(buf) + 1;
		if (buf >= ebuf)
			return 0;
	} else
		very = 0;
	if ((m = findmod(lighttable, COUNT(lighttable), buf))) {
		if (strcmp(buf, "pale") == 0 || strcmp(buf, "deep") == 0)
			very = 1;
		buf += strlen(buf) + 1;
		if (buf >= ebuf)
			return 0;
		if (m->val != 50)
			lgt = m->val / 100.0;
	} else if (very)
		return 0;
	if ((m = findmod(sattable, COUNT(sattable), buf))) {
		sat = m->val / 100.0;
		buf += strlen(buf) + 1;
		if (buf >= ebuf)
			return 0;
	}
	if (buf + strlen(buf) >= ebuf)
		blend = h1 = l1 = s1 = 0.0;
	else {
		if ((cn = findname(buf, false)))
			blend = 0.5;
		else if ((cn = findname(buf, true)))
			blend = 0.25;
		else
			return 0;
		h1 = cn->hue;
		l1 = cn->lgt / 100.0;
		s1 = cn->sat / 100.0;
		buf += strlen(buf) + 1;
	}
	cn = findname(buf, false);
	if (!cn || buf + strlen(buf) < ebuf)
		return 0;
	h2 = cn->hue;
	l2 = cn->lgt / 100.0;
	s2 = cn->sat / 100.0;
	if (blend > 0) {
		bl2 = 1.0 - blend;
		if (s1 == 0.0)
			;
		else if (s2 == 0.0)
			h2 = h1;
		else if (h2 - h1 > 180)
			h2 = blend * h1 + bl2 * (h2 - 360);
		else if (h1 - h2 > 180)
			h2 = blend * (h1 - 360) + bl2 * h2;
		else
			h2 = blend * h1 + bl2 * h2;
		if (h2 < 0)
			h2 += 360;
		l2 = blend * l1 + bl2 * l2;
		s2 = blend * s1 + bl2 * s2;
	}
	if (lgt >= 0.0) {
		if (very)
			l2 = (2 * lgt + l2) / 3.0;
		else
			l2 = (lgt + 2 * l2) / 3.0;
	}
	s2 *= sat;
	if (s2 == 0)
		r2 = g2 = b2 = l2;
	else {
		if (l2 < 0.5)
			m2 = l2 * (1 + s2);
		else
			m2 = l2 + s2 - l2 * s2;
		m1 = 2 * l2 - m2;
		r2 = rgbval(m1, m2, h2 + 120);
		g2 = rgbval(m1, m2, h2);
		b2 = rgbval(m1, m2, h2 - 120);
	}
	*r = 65535 * r2;
	*g = 65535 * g2;
	*b = 65535 * b2;
	return 1;
}

/* X's colour names, from the server, remembered. */
typedef struct XName { char *name; int ok, r, g, b; struct XName *next; } XName;
static XName *xnames;

static int xcolor(const char *s, int *r, int *g, int *b) {
	for (XName *x = xnames; x; x = x->next)
		if (!strcmp(x->name, s)) {
			*r = x->r, *g = x->g, *b = x->b;
			return x->ok;
		}
	if (!gp_connect())
		return 0;
	gp_begin(P_COLOR);
	gp_str(s, strlen(s));
	gp_end();
	gp_reply(P_COLOR);
	XName *x = xcalloc(1, sizeof *x);
	x->name = salloc(s);
	x->ok = gr_i32();
	x->r = gr_i32(), x->g = gr_i32(), x->b = gr_i32();
	x->next = xnames;
	xnames = x;
	*r = x->r, *g = x->g, *b = x->b;
	return x->ok;
}

/* nativecolor: X's colour names, linear as rxwin.ri's lcolor makes
 * them (rsdlwin.ri). */
static int nativecolor(WBinding *w, const char *s, long *r, long *g, long *b) {
	int R, G, B;
	if (!w || !xcolor(s, &R, &G, &B))
		return 0;
	double gamma = w->context->gamma;
	*r = 65535 * pow((R * 257) / 65535.0, gamma);
	*g = 65535 * pow((G * 257) / 65535.0, gamma);
	*b = 65535 * pow((B * 257) / 65535.0, gamma);
	return 1;
}

static int parsecolor(WBinding *w, const char *buf, long *r, long *g, long *b) {
	int len, mul;
	const char *fmt;
	char c;
	double dr, dg, db;

	*r = *g = *b = 0L;
	while (isspace((unsigned char)*buf))
		buf++;
	if (sscanf(buf, "%lf,%lf,%lf%c", &dr, &dg, &db, &c) == 3) {
		*r = dr;
		*g = dg;
		*b = db;
		if (*r >= 0 && *r <= 65535 && *g >= 0 && *g <= 65535 && *b >= 0 && *b <= 65535)
			return Succeeded;
		return Failed;
	}
	if (*buf == '#') {
		buf++;
		for (len = 0; isalnum((unsigned char)buf[len]); len++)
			;
		switch (len) {
		case 3: fmt = "%1lx%1lx%1lx%c"; mul = 0x1111; break;
		case 6: fmt = "%2lx%2lx%2lx%c"; mul = 0x0101; break;
		case 9: fmt = "%3lx%3lx%3lx%c"; mul = 0x0010; break;
		case 12: fmt = "%4lx%4lx%4lx%c"; mul = 0x0001; break;
		default: return Failed;
		}
		if (sscanf(buf, fmt, r, g, b, &c) != 3)
			return Failed;
		*r *= mul;
		*g *= mul;
		*b *= mul;
		return Succeeded;
	}
	if (colorphrase(buf, r, g, b) || nativecolor(w, buf, r, g, b))
		return Succeeded;
	return Failed;
}

/* Device colours remembered as X would (rsdlrsc.ri): the X colour of
 * every pixel value handed out, the first allocation winning, and the
 * first name each X colour was asked for by. */
enum { CTSIZE = 4096 };
typedef struct CtEntry { unsigned pix; unsigned short r, g, b; struct CtEntry *next; } CtEntry;
typedef struct NEntry { unsigned short r, g, b; char name[6 + MAXCOLORNAME]; struct NEntry *next; } NEntry;
static CtEntry *ctable[CTSIZE];
static NEntry *ntable[CTSIZE];

static CtEntry *ct_find(unsigned pix) {
	for (CtEntry *e = ctable[(pix ^ (pix >> 12)) % CTSIZE]; e; e = e->next)
		if (e->pix == pix)
			return e;
	return NULL;
}

static void ct_add(unsigned pix, unsigned r, unsigned g, unsigned b) {
	if (ct_find(pix))
		return;
	CtEntry *e = xmalloc(sizeof *e);
	int h = (int)((pix ^ (pix >> 12)) % CTSIZE);
	e->pix = pix, e->r = (unsigned short)r, e->g = (unsigned short)g, e->b = (unsigned short)b;
	e->next = ctable[h];
	ctable[h] = e;
}

static char *colour_name(unsigned r, unsigned g, unsigned b, const char *name) {
	int h = (int)((503 * r + 509 * g + 499 * b) % CTSIZE);
	for (NEntry *e = ntable[h]; e; e = e->next)
		if (e->r == r && e->g == g && e->b == b)
			return e->name;
	if (!name)
		return NULL;
	NEntry *e = xmalloc(sizeof *e);
	e->r = (unsigned short)r, e->g = (unsigned short)g, e->b = (unsigned short)b;
	strncpy(e->name, name, sizeof e->name - 1);
	e->name[sizeof e->name - 1] = 0;
	e->next = ntable[h];
	ntable[h] = e;
	return e->name;
}

static unsigned sdl_rgb(WBinding *w, long r, long g, long b, const char *name, char *nameout) {
	unsigned short xr, xg, xb;
	unsigned pix;
	double invgamma = 1.0 / w->context->gamma;
	static bool inited;
	if (!inited) {
		ct_add(0x000000, 0, 0, 0);
		ct_add(0xFFFFFF, 65535, 65535, 65535);
		colour_name(0, 0, 0, "black");
		colour_name(65535, 65535, 65535, "white");
		inited = true;
	}
	if (r == 0 && g == 0 && b == 0) {
		xr = xg = xb = 0;
		pix = 0x000000;
	} else if (r == 65535 && g == 65535 && b == 65535) {
		xr = xg = xb = 65535;
		pix = 0xFFFFFF;
	} else {
		xr = 65535 * pow(r / 65535.0, invgamma);
		xg = 65535 * pow(g / 65535.0, invgamma);
		xb = 65535 * pow(b / 65535.0, invgamma);
		pix = (unsigned)((xr >> 8) << 16) | (unsigned)((xg >> 8) << 8) | (unsigned)(xb >> 8);
		ct_add(pix, xr, xg, xb);
	}
	char *nm = colour_name(xr, xg, xb, name);
	if (nameout) {
		strncpy(nameout, nm ? nm : "", 6 + MAXCOLORNAME - 1);
		nameout[6 + MAXCOLORNAME - 1] = 0;
	}
	return pix;
}

static void sdl_lcolor(WBinding *w, unsigned pix, long *r, long *g, long *b) {
	CtEntry *e = ct_find(pix);
	unsigned xr, xg, xb;
	double gamma = w->context->gamma;
	if (e) {
		xr = e->r, xg = e->g, xb = e->b;
	} else {
		xr = ((pix >> 16) & 255) * 257;
		xg = ((pix >> 8) & 255) * 257;
		xb = (pix & 255) * 257;
	}
	*r = 65535 * pow((int)xr / 65535.0, gamma);
	*g = 65535 * pow((int)xg / 65535.0, gamma);
	*b = 65535 * pow((int)xb / 65535.0, gamma);
}

/* X's effective default gamma (rsdlrsc.ri's sdl_gamma). */
static double sdl_gamma(void) {
	double g = .299 * 49408 + .587 * 47872 + .114 * 47872;
	g /= 65535;
	return log(0.5) / log(g);
}

static int setfg(WBinding *w, const char *s) {
	long r, g, b;
	if (parsecolor(w, s, &r, &g, &b) != Succeeded)
		return Failed;
	w->context->fg = sdl_rgb(w, r, g, b, s, w->context->fgname);
	return Succeeded;
}

static int setbg(WBinding *w, const char *s) {
	long r, g, b;
	if (parsecolor(w, s, &r, &g, &b) != Succeeded)
		return Failed;
	w->context->bg = sdl_rgb(w, r, g, b, s, w->context->bgname);
	return Succeeded;
}

static int isetcolor(WBinding *w, int64_t c, bool fg) {
	char s[64];
	if (c < 0)
		return Failed; /* no mutable colours */
	snprintf(s, sizeof s, "%d,%d,%d", (int)((c >> 16) & 255) * 257, (int)((c >> 8) & 255) * 257,
		(int)(c & 255) * 257);
	return fg ? setfg(w, s) : setbg(w, s);
}

static void toggle_fgbg(WBinding *w) {
	WContext *wc = w->context;
	unsigned t = wc->fg;
	char tn[6 + MAXCOLORNAME];
	wc->fg = wc->bg;
	wc->bg = t;
	strcpy(tn, wc->fgname);
	strcpy(wc->fgname, wc->bgname);
	strcpy(wc->bgname, tn);
}

static int setgamma(WBinding *w, double gamma) {
	char fg[6 + MAXCOLORNAME], bg[6 + MAXCOLORNAME];
	w->context->gamma = gamma;
	strcpy(fg, w->context->fgname);
	strcpy(bg, w->context->bgname);
	setfg(w, fg);
	setbg(w, bg);
	return Succeeded;
}

/* ---- Fonts (rwindow.r's parsefont; rsdlrsc.ri's alc_font) ---- */

enum {
	FONTATT_SPACING = 0x01000000, FONTFLAG_MONO = 0x1, FONTFLAG_PROPORTIONAL = 0x2,
	FONTATT_SERIF = 0x02000000, FONTFLAG_SANS = 0x4, FONTFLAG_SERIF = 0x8,
	FONTATT_SLANT = 0x04000000, FONTFLAG_ROMAN = 0x10, FONTFLAG_ITALIC = 0x20,
	FONTFLAG_OBLIQUE = 0x40, FONTATT_WEIGHT = 0x08000000, FONTFLAG_LIGHT = 0x100,
	FONTFLAG_MEDIUM = 0x200, FONTFLAG_DEMI = 0x400, FONTFLAG_BOLD = 0x800,
	FONTATT_WIDTH = 0x10000000, FONTFLAG_CONDENSED = 0x1000, FONTFLAG_NARROW = 0x2000,
	FONTFLAG_NORMAL = 0x4000, FONTFLAG_WIDE = 0x8000, FONTFLAG_EXTENDED = 0x10000,
};

static const StrInt fontwords[] = {
	{"bold", FONTATT_WEIGHT | FONTFLAG_BOLD},
	{"condensed", FONTATT_WIDTH | FONTFLAG_CONDENSED},
	{"demi", FONTATT_WEIGHT | FONTFLAG_DEMI},
	{"demibold", FONTATT_WEIGHT | FONTFLAG_DEMI | FONTFLAG_BOLD},
	{"extended", FONTATT_WIDTH | FONTFLAG_EXTENDED},
	{"italic", FONTATT_SLANT | FONTFLAG_ITALIC},
	{"light", FONTATT_WEIGHT | FONTFLAG_LIGHT},
	{"medium", FONTATT_WEIGHT | FONTFLAG_MEDIUM},
	{"mono", FONTATT_SPACING | FONTFLAG_MONO},
	{"narrow", FONTATT_WIDTH | FONTFLAG_NARROW},
	{"normal", FONTATT_WIDTH | FONTFLAG_NORMAL},
	{"oblique", FONTATT_SLANT | FONTFLAG_OBLIQUE},
	{"proportional", FONTATT_SPACING | FONTFLAG_PROPORTIONAL},
	{"roman", FONTATT_SLANT | FONTFLAG_ROMAN},
	{"sans", FONTATT_SERIF | FONTFLAG_SANS},
	{"serif", FONTATT_SERIF | FONTFLAG_SERIF},
	{"wide", FONTATT_WIDTH | FONTFLAG_WIDE},
};

static int parsefont(const char *s, char family[MAXFONTWORD + 1], int *style, int *size) {
	char c, *a, attr[MAXFONTWORD + 1];
	int tmp;
	*family = '\0';
	*style = 0;
	*size = -1;
	for (;;) {
		while (isspace((unsigned char)*s) || *s == ',')
			s++;
		if (*s == '\0')
			break;
		for (a = attr; (c = *s) != '\0' && c != ','; s++) {
			if (isupper((unsigned char)c))
				c = (char)tolower((unsigned char)c);
			*a++ = c;
			if (a - attr >= MAXFONTWORD)
				return 0;
		}
		while (a > attr && isspace((unsigned char)a[-1]))
			a--;
		*a = '\0';
		if (*family == '\0')
			strcpy(family, attr);
		else if (sscanf(attr, "%d%c", &tmp, &c) == 1 && tmp > 0) {
			if (*size != -1 && *size != tmp)
				return 0;
			*size = tmp;
		} else {
			tmp = si_s2i(fontwords, COUNT(fontwords), attr);
			if (tmp != -1) {
				if ((tmp & *style) != 0 && (tmp & *style) != tmp)
					return 0;
				*style |= tmp;
			}
		}
	}
	return *family != '\0';
}

static WFont *alc_font(WBinding *w, const char *s) {
	char family[MAXFONTWORD + 1];
	const char *fam;
	int flags, size;
	for (WFont *f = fonts; f; f = f->next)
		if (!strcmp(f->name, s))
			return f;
	if (!strcmp(s, "fixed")) {
		fam = "fixed";
		flags = 0;
		size = 11;
	} else {
		if (!parsefont(s, family, &flags, &size))
			return NULL;
		if (size <= 0)
			size = DEFAULTFONTSIZE;
		fam = family;
	}
	if (!gp_connect())
		return NULL;
	uint32_t id = ++lastfont;
	gp_begin(P_FONT);
	gp_u32(id);
	gp_str(fam, strlen(fam));
	gp_u8((flags & (FONTFLAG_BOLD | FONTFLAG_DEMI)) != 0);
	gp_u8((flags & (FONTFLAG_ITALIC | FONTFLAG_OBLIQUE)) != 0);
	gp_i32(size);
	gp_end();
	gp_reply(P_FONT);
	if (!gr_i32())
		return NULL;
	WFont *f = xcalloc(1, sizeof *f);
	f->name = salloc(s);
	f->id = id;
	f->ascent = gr_i32();
	f->descent = gr_i32();
	f->fwidth = gr_i32();
	f->height = f->ascent + f->descent;
	f->next = fonts;
	fonts = f;
	/* As X's tryfont: loading a font sets the context's leading. */
	if (w && w->context)
		w->context->leading = f->height;
	return f;
}

static int setfont(WBinding *w, const char *s) {
	WFont *f = alc_font(w, s);
	if (!f)
		return Failed;
	w->context->font = f;
	if (!w->window->id) {
		w->window->y = ASCENT(w);
		w->window->x = 0;
	}
	return Succeeded;
}

static int textWidth(WBinding *w, const char *s, size_t n) {
	if (n == 0)
		return 0;
	gp_begin(P_WIDTH);
	gp_u32(w->context->font->id);
	gp_str(s, n);
	gp_end();
	gp_reply(P_WIDTH);
	return gr_i32();
}

/* ---- Contexts and window states (rsdlrsc.ri) ---- */

static WContext *alc_context(WBinding *w) {
	WFont *f = alc_font(w, "fixed");
	if (!f)
		return NULL;
	WContext *wc = xcalloc(1, sizeof *wc);
	wc->refcount = 1;
	wc->serial = ++context_serial;
	wc->gc = ++lastgc;
	wc->gamma = sdl_gamma();
	wc->fg = 0x000000;
	strcpy(wc->fgname, "black");
	wc->bg = 0xFFFFFF;
	strcpy(wc->bgname, "white");
	wc->font = f;
	wc->leading = f->height;
	wc->drawop = DRAWOP_COPY;
	wc->clipw = wc->cliph = -1;
	wc->linestyle = LS_SOLID;
	wc->linewidth = 1;
	wc->fillstyle = FS_SOLID;
	wc->next = wcntxts;
	wcntxts = wc;
	return wc;
}

static WContext *clone_context(WBinding *w) {
	WContext *wc = w->context, *rv = alc_context(w);
	if (!rv)
		return NULL;
	rv->dx = wc->dx, rv->dy = wc->dy;
	rv->clipx = wc->clipx, rv->clipy = wc->clipy;
	rv->clipw = wc->clipw, rv->cliph = wc->cliph;
	rv->fg = wc->fg;
	strcpy(rv->fgname, wc->fgname);
	rv->bg = wc->bg;
	strcpy(rv->bgname, wc->bgname);
	rv->font = wc->font;
	rv->leading = wc->leading;
	rv->fillstyle = wc->fillstyle;
	rv->linestyle = wc->linestyle;
	rv->linewidth = wc->linewidth;
	rv->drawop = wc->drawop;
	rv->gamma = wc->gamma;
	rv->bits = wc->bits;
	if (wc->patternname)
		rv->patternname = salloc(wc->patternname);
	if (wc->pat) {
		rv->pat = xmalloc((size_t)wc->patw * (size_t)wc->path);
		memcpy(rv->pat, wc->pat, (size_t)wc->patw * (size_t)wc->path);
		rv->patw = wc->patw, rv->path = wc->path;
	}
	return rv;
}

static WState *alc_winstate(void) {
	WState *ws = xcalloc(1, sizeof *ws);
	ws->refcount = 1;
	ws->serial = ++canvas_serial;
	ws->bits = 1024; /* echo on */
	ws->iconic = NormalState;
	ws->posx = ws->posy = -MaxInt;
	ws->next = wstates;
	wstates = ws;
	return ws;
}

static void unmap(WState *ws) {
	if (ws->win) {
		ws->win = false;
		nwins--;
	}
}

static void free_window(WState *ws) {
	if (--ws->refcount > 0)
		return;
	free(ws->initpx);
	free(ws->initop);
	free(ws->iconpx);
	ws->initpx = ws->iconpx = NULL;
	ws->initop = NULL;
	ws->bits |= 1; /* closed */
	unmap(ws);
	if (ws->id) {
		gp_begin(P_FREE);
		gp_u32(ws->id);
		gp_end();
		ws->id = 0;
	}
	for (WState **p = &wstates; *p; p = &(*p)->next)
		if (*p == ws) {
			*p = ws->next;
			break;
		}
}

/* ---- Drawing through the protocol (rsdlwin.ri) ---- */

/* The parts of a context the server's gc holds, as sent last. */
typedef struct GcState {
	unsigned fg, bg;
	int drawop, reverse, linewidth, linestyle, fillstyle;
	int clipx, clipy, clipw, cliph, patw, path;
	const unsigned char *pat;
} GcState;

typedef struct SentGc { uint32_t id; GcState g; unsigned char *pat; struct SentGc *next; } SentGc;
static SentGc *sentgcs;

static void mkgc(WBinding *w, GcState *g) {
	WContext *wc = w->context;
	memset(g, 0, sizeof *g);
	g->fg = wc->fg, g->bg = wc->bg;
	g->drawop = wc->drawop;
	g->reverse = ISXORREVERSE(w) != 0;
	g->linewidth = wc->linewidth;
	g->linestyle = wc->linestyle;
	g->fillstyle = wc->fillstyle;
	g->patw = wc->patw, g->path = wc->path, g->pat = wc->pat;
	g->clipx = wc->clipx, g->clipy = wc->clipy, g->clipw = wc->clipw, g->cliph = wc->cliph;
}

static bool samegc(const SentGc *s, const GcState *g) {
	const GcState *o = &s->g;
	if (o->fg != g->fg || o->bg != g->bg || o->drawop != g->drawop || o->reverse != g->reverse ||
	    o->linewidth != g->linewidth || o->linestyle != g->linestyle ||
	    o->fillstyle != g->fillstyle || o->clipx != g->clipx || o->clipy != g->clipy ||
	    o->clipw != g->clipw || o->cliph != g->cliph || o->patw != g->patw || o->path != g->path)
		return false;
	size_t k = (size_t)g->patw * (size_t)g->path;
	return k == 0 || !memcmp(s->pat, g->pat, k);
}

/* usegc makes the server's gc id hold g, sending it if it does not. */
static uint32_t usegc(uint32_t id, const GcState *g) {
	SentGc *s;
	for (s = sentgcs; s && s->id != id; s = s->next)
		;
	if (s && samegc(s, g))
		return id;
	if (!s) {
		s = xcalloc(1, sizeof *s);
		s->id = id;
		s->next = sentgcs;
		sentgcs = s;
	}
	size_t k = (size_t)g->patw * (size_t)g->path;
	s->g = *g;
	free(s->pat);
	s->pat = NULL;
	if (k) {
		s->pat = xmalloc(k);
		memcpy(s->pat, g->pat, k);
	}
	gp_begin(P_GC);
	gp_u32(id);
	gp_u32(g->fg);
	gp_u32(g->bg);
	gp_u8(g->drawop);
	gp_u8(g->reverse);
	gp_i32(g->linewidth);
	gp_u8(g->linestyle);
	gp_u8(g->fillstyle);
	gp_i32(g->clipx);
	gp_i32(g->clipy);
	gp_i32(g->clipw);
	gp_i32(g->cliph);
	gp_i32(k ? g->patw : 0);
	gp_i32(k ? g->path : 0);
	if (k)
		gp_bytes(g->pat, k);
	gp_end();
	return id;
}

static uint32_t wgc(WBinding *w) {
	GcState g;
	mkgc(w, &g);
	return usegc(w->context->gc, &g);
}

/* A gc for one call that changes the context's (CopyArea, write). */
enum { TempGc = 0xFFFFFFFF };

static void show(bool full);

/* drawn counts drawing requests; every so often the windows are shown
 * and events collected, as iconx's pollevent does. */
static int drawn;
static int pollevent(void);

static void drawing(void) {
	if (++drawn >= 64) {
		drawn = 0;
		if (nwins && millisec() - lastshow >= 100)
			pollevent();
	}
}

static void drawxy(WBinding *w, int op, const int *xy, int n, int k) {
	if (!w->window->id || n <= 0)
		return;
	uint32_t gc = wgc(w);
	gp_begin(op);
	gp_u32(w->window->id);
	gp_u32(gc);
	gp_u32((uint32_t)n);
	for (int i = 0; i < n * k; i++)
		gp_i32(xy[i]);
	gp_end();
	drawing();
}

typedef struct { int x, y, width, height; double angle1, angle2; } XArc;

static void drawarcs(WBinding *w, const XArc *arcs, int n, bool fill) {
	if (!w->window->id || n <= 0)
		return;
	uint32_t gc = wgc(w);
	gp_begin(P_ARCS);
	gp_u32(w->window->id);
	gp_u32(gc);
	gp_u8(fill);
	gp_u32((uint32_t)n);
	for (int i = 0; i < n; i++) {
		gp_i32(arcs[i].x);
		gp_i32(arcs[i].y);
		gp_i32(arcs[i].width);
		gp_i32(arcs[i].height);
		gp_f64(arcs[i].angle1);
		gp_f64(arcs[i].angle2);
	}
	gp_end();
	drawing();
}

static void erase_gc(WState *ws, uint32_t gc, int x, int y, int width, int height) {
	gp_begin(P_ERASE);
	gp_u32(ws->id);
	gp_u32(gc);
	gp_i32(x);
	gp_i32(y);
	gp_i32(width);
	gp_i32(height);
	gp_end();
}

/* eraseArea: an area reaching the window's edge extends over the whole
 * canvas, as rxwin.ri's. */
static void eraseArea(WBinding *w, int x, int y, int width, int height) {
	WState *ws = w->window;
	if (!ws->id)
		return;
	if (x + width >= ws->width)
		width = ws->pixwidth - x;
	if (y + height >= ws->height)
		height = ws->pixheight - y;
	erase_gc(ws, wgc(w), x, y, width, height);
	drawing();
}

/* copyArea, as rsdlwin.ri's (from rxwin.ri): copy mode, w2's clip, and
 * the part of the source off its canvas filled with w2's background. */
static int copyArea(WBinding *w, WBinding *w2, int x, int y, int width, int height, int x2, int y2) {
	WState *ws1 = w->window;
	int lpad, rpad, tpad, bpad;
	if (!w->window->id || !w2->window->id)
		return Failed;
	GcState g;
	mkgc(w2, &g);
	g.drawop = DRAWOP_COPY;
	g.reverse = 0;
	g.fillstyle = FS_SOLID;
	uint32_t gc = usegc(TempGc, &g);
	WState *ws2 = w2->window;
	if (x + width < 0 || y + height < 0 || x >= ws1->pixwidth || y >= ws1->pixheight) {
		erase_gc(ws2, gc, x2, y2, width, height);
		return Succeeded;
	}
	lpad = rpad = tpad = bpad = 0;
	if (x < 0) {
		lpad = -x;
		width -= lpad;
		x2 += lpad;
		x = 0;
	}
	if (x + width > ws1->pixwidth) {
		rpad = x + width - ws1->pixwidth;
		width -= rpad;
	}
	if (y < 0) {
		tpad = -y;
		height -= tpad;
		y2 += tpad;
		y = 0;
	}
	if (y + height > ws1->pixheight) {
		bpad = y + height - ws1->pixheight;
		height -= bpad;
	}
	gp_begin(P_COPY);
	gp_u32(ws1->id);
	gp_u32(ws2->id);
	gp_u32(gc);
	gp_i32(x);
	gp_i32(y);
	gp_i32(width);
	gp_i32(height);
	gp_i32(x2);
	gp_i32(y2);
	gp_end();
	if (lpad > 0)
		erase_gc(ws2, gc, x2 - lpad, y2 - tpad, lpad, tpad + height + bpad);
	if (rpad > 0)
		erase_gc(ws2, gc, x2 + width, y2 - tpad, rpad, tpad + height + bpad);
	if (tpad > 0)
		erase_gc(ws2, gc, x2, y2 - tpad, width, tpad);
	if (bpad > 0)
		erase_gc(ws2, gc, x2, y2 + height, width, bpad);
	drawing();
	return Succeeded;
}

/* text: DrawString (image false), or write's text at the cursor, its
 * cell first filled with bg (image true). */
static void text(WBinding *w, int x, int y, const char *s, size_t n, bool image) {
	if (!w->window->id || n == 0)
		return;
	uint32_t gc = wgc(w);
	gp_begin(P_TEXT);
	gp_u32(w->window->id);
	gp_u32(gc);
	gp_u32(w->context->font->id);
	gp_i32(x);
	gp_i32(y);
	gp_u8(image);
	gp_str(s, n);
	gp_end();
	drawing();
}

/* ---- Writing to a window (rwindow.r's wputstr, rsdlwin.ri's wputc
 * and xdis) ---- */

/* xdis draws text at the text position with its background, and
 * advances the position. */
static void xdis(WBinding *w, const char *s, size_t n) {
	WState *ws = w->window;
	text(w, ws->x, ws->y, s, n, true);
	ws->x += textWidth(w, s, n);
}

/* rawgc is w's context as a gc in copy mode without clipping, for
 * scrolling and clearing to the end of the line. */
static uint32_t rawgc(WBinding *w) {
	GcState g;
	mkgc(w, &g);
	g.drawop = DRAWOP_COPY;
	g.reverse = 0;
	g.fillstyle = FS_SOLID;
	g.clipw = -1;
	return usegc(TempGc, &g);
}

/* evquesub is the keystroke i places back (i < 0) from the newest. */
static char *evquesub(WState *ws, int i) {
	int j = ws->eQback + i;
	if (j < 0)
		j += EQUEUELEN;
	else if (j >= EQUEUELEN)
		j -= EQUEUELEN;
	return &ws->eventQueue[j];
}

/* wputc writes a character that is not printable: newline (with
 * scrolling), return, tab, and backspace and delete, which erase the
 * last character typed. */
static void wputc(WBinding *w, char c) {
	WState *ws = w->window;
	WContext *wc = w->context;
	int lh = wc->leading, width = ws->width, height = ws->height;
	switch (c) {
	case '\r':
		ws->x = wc->dx;
		break;
	case '\n': {
		if (ISCEOLON(w) && ws->id) /* clear to the end of the line */
			erase_gc(ws, rawgc(w), ws->x, ws->y - ASCENT(w), width - ws->x, lh);
		ws->y += lh;
		ws->x = wc->dx;
		int over = ws->y + DESCENT(w) - height;
		if (over > 0) { /* scroll */
			ws->y -= over;
			if (ws->id) {
				uint32_t gc = rawgc(w);
				gp_begin(P_COPY);
				gp_u32(ws->id);
				gp_u32(ws->id);
				gp_u32(gc);
				gp_i32(0);
				gp_i32(over);
				gp_i32(width);
				gp_i32(height - over);
				gp_i32(0);
				gp_i32(0);
				gp_end();
				erase_gc(ws, gc, 0, height - over, width, over);
			}
		}
		drawing();
		break;
	}
	case '\t':
		xdis(w, "        ", (size_t)(8 - (XTOCOL(w, ws->x) & 7)));
		break;
	case '\177':
	case '\010': {
		int i = -1;
		while (i > -EQUEUELEN && *evquesub(ws, i) != c)
			i--;
		if (i == -EQUEUELEN)
			break;
		i--;
		while (i > -EQUEUELEN && !isprint((unsigned char)*evquesub(ws, i)))
			i--;
		if (i == -EQUEUELEN)
			break;
		char prev = *evquesub(ws, i);
		int prex = ws->x;
		ws->x -= textWidth(w, &prev, 1);
		*evquesub(ws, i) = '\010';
		int x = ws->x;
		while (ws->x < prex)
			xdis(w, " ", 1);
		ws->x = x;
		break;
	}
	default:
		xdis(w, &c, 1);
	}
}

/* gfx_write writes s to window f at its text position, as write()
 * does (wputstr): runs of printable characters drawn as text, the
 * others interpreted. */
void gfx_write(File *f, const char *s, size_t n) {
	WBinding *w = f->gfx;
	const char *p = s;
	while (n > 0) {
		/* printable: ASCII graphic characters, and any non-ASCII byte
		 * (part of a UTF-8 text character) */
		while (n > 0 && (isprint((unsigned char)*p) || (unsigned char)*p >= 0x80))
			p++, n--;
		if (p != s)
			xdis(w, s, (size_t)(p - s));
		if (n > 0) {
			wputc(w, *p++);
			n--;
		}
		s = p;
	}
}

/* ---- Showing, and events (rsdlwin.ri, rwindow.r) ---- */

static void show(bool full) {
	if (!nwins)
		return;
	gp_begin(P_SHOW);
	gp_u8(full);
	gp_end();
	gp_flush();
	lastshow = millisec();
}

static void wflush(void) { show(false); }

/* qevent puts an event on ws's Icon list: the code, then two integers
 * packing x, y, the modifier keys and the interval (rwindow.r). */
static void qevent(WState *ws, Value e, int x, int y, uint64_t t, long f) {
	int64_t ivl, mod = 0;
	if (f & ControlMask)
		mod |= EQ_MOD_CONTROL;
	if (f & Mod1Mask)
		mod |= EQ_MOD_META;
	if (f & ShiftMask)
		mod |= EQ_MOD_SHIFT;
	if (t != ~(uint64_t)0) {
		if (ws->timestamp == 0)
			ws->timestamp = t;
		if (t < ws->timestamp)
			t = ws->timestamp;
		ivl = (int64_t)(t - ws->timestamp);
		ws->timestamp = t;
		int64_t expo = 0;
		while (ivl >= 0x1000) {
			ivl >>= 4;
			expo += 0x1000;
		}
		ivl += expo;
	} else
		ivl = 0;
	list_append(ws->listp, e);
	list_append(ws->listp, intval(mod | (x & 0xFFFF)));
	list_append(ws->listp, intval((ivl << 16) | (y & 0xFFFF)));
}

static int buttonorder[3] = {1, 2, 3};
static int presscount;

static void btn_front(int b) {
	int i, j;
	for (i = 0; i < 3 && buttonorder[i] != b; i++)
		;
	if (i == 3)
		i = 2;
	for (j = i; j > 0; j--)
		buttonorder[j] = buttonorder[j - 1];
	buttonorder[0] = b;
}

static void btn_back(int b) {
	int i, j;
	for (i = 0; i < 3 && buttonorder[i] != b; i++)
		;
	if (i == 3)
		i = 2;
	for (j = i; j < 2; j++)
		buttonorder[j] = buttonorder[j + 1];
	buttonorder[2] = b;
}

static WBinding *bindingof(WState *ws); /* a binding of ws, for its context */
static void putpixels(WBinding *w, int x, int y, int wd, int ht, const unsigned *px, const unsigned char *op);
static bool loadimage(WBinding *w, const char *name, int p, unsigned **px, unsigned char **op, int *iw, int *ih);
static void sendicon(WState *ws);
static void opened(File *f);

static void do_resize(WState *ws, int w, int h, unsigned bg) {
	gp_begin(P_RESIZE);
	gp_u32(ws->id);
	gp_i32(w);
	gp_i32(h);
	gp_u32(bg);
	gp_end();
}

/* process_events asks the server for events (waiting up to timeout
 * milliseconds) and puts them on the windows' lists, as rsdlwin.ri's
 * process_events. -1 if the user closed a window (error 141). */
static int process_events(int timeout) {
	int rv = 1;
	gp_begin(P_WAIT);
	gp_i32(timeout);
	gp_end();
	gp_reply(P_WAIT);
	lastshow = millisec();
	uint32_t count = gr_u32();
	for (uint32_t k = 0; k < count; k++) {
		uint32_t id = gr_u32();
		int kind = gr_i32();
		int64_t code = gr_i64();
		int x = gr_i32(), y = gr_i32();
		long mods = (long)gr_i64();
		uint64_t ms = (uint64_t)gr_i64();
		WState *ws;
		for (ws = wstates; ws && !(ws->win && ws->id == id); ws = ws->next)
			;
		if (!ws)
			continue;
		switch (kind) {
		case PL_KEYCHAR: {
			char c = (char)(code & 0xFF);
			ws->pointerx = x, ws->pointery = y;
			/* copied: mkstr would keep a pointer to c, on the stack */
			qevent(ws, strval(strcopy(&c, 1)), x, y, ms, mods);
			break;
		}
		case PL_KEYSYM:
			ws->pointerx = x, ws->pointery = y;
			qevent(ws, intval(code), x, y, ms, mods);
			break;
		case PL_PRESS:
		case PL_RELEASE: {
			int c;
			if (kind == PL_PRESS) {
				presscount++;
				btn_front((int)code);
				c = MOUSELEFT - (int)(code - 1);
			} else {
				if (--presscount < 0)
					presscount = 0;
				btn_back((int)code);
				c = MOUSELEFTUP - (int)(code - 1);
			}
			ws->pointerx = x, ws->pointery = y;
			qevent(ws, intval(c), x, y, ms, mods);
			break;
		}
		case PL_MOTION:
			ws->pointerx = x, ws->pointery = y;
			if (presscount == 0)
				break;
			qevent(ws, intval(MOUSELEFTDRAG - (buttonorder[0] - 1)), x, y, ms, mods);
			break;
		case PL_RESIZE: {
			WBinding *wb = bindingof(ws);
			if (!wb)
				break;
			if (ws->x > x - FWIDTH(wb))
				ws->x = x - FWIDTH(wb);
			if (ws->y > y)
				ws->y = y;
			if (x == ws->width && y == ws->height)
				break;
			ws->width = x;
			ws->height = y;
			do_resize(ws, x, y, wb->context->bg);
			if (x > ws->pixwidth)
				ws->pixwidth = x;
			if (y > ws->pixheight)
				ws->pixheight = y;
			if (ISEXPOSED(wb))
				qevent(ws, intval(RESIZED), ws->width, ws->height, ~(uint64_t)0, 0);
			break;
		}
		case PL_MOVE:
			ws->posx = x;
			ws->posy = y;
			break;
		case PL_EXPOSE:
			ws->bits |= 256;
			break;
		case PL_STATE:
			ws->iconic = code == PL_ICONIC ? IconicState : code == PL_MAXIMAL ? MaximizedState : NormalState;
			break;
		case PL_CLOSE:
			rv = -1;
			break;
		}
	}
	return rv;
}

/* pollevent shows the windows and collects their events (iconx's
 * pollevent); -1 if one was closed by the user. */
static int pollevent(void) {
	if (nwins == 0)
		return 1;
	return process_events(0);
}

/* evqueget takes the next value from ws's list, recording keystrokes
 * for backspace (EVQUEGET). */
static bool evqueget(WState *ws, Value *d) {
	List *l = ws->listp;
	if (l->n == 0)
		return false;
	*d = l->e[l->off];
	l->off++;
	l->n--;
	if (VKIND(*d) == KString && VN(*d) > 0) {
		ws->eventQueue[ws->eQfront++] = *vstr(*d).s;
		if (ws->eQfront >= EQUEUELEN)
			ws->eQfront = 0;
		ws->eQback = ws->eQfront;
	}
	return true;
}

/* wgetq gets the next value of w's event list, waiting for one if need
 * be (showing everything first); -1 if the window is gone. A hidden
 * canvas never receives an event: it is reported as closed. */
static int wgetq(WBinding *w, Value *res) {
	WState *ws = w->window;
	bool posted = false;
	int cx = 0, cy = 0, cw = 0, ch = 0;
	for (;;) {
		if (nwins && process_events(0) < 0)
			return -1;
		if (ws->listp->n > 0) {
			evqueget(ws, res);
			break;
		}
		if (!ws->win)
			return -1;
		if (ISCURSORON(w) && ws->id) {
			/* the text cursor: a fg bar under the text position,
			 * on the window but not its canvas */
			cx = ws->x, cy = ws->y, cw = FWIDTH(w), ch = DESCENT(w);
			gp_begin(P_OVERLAY);
			gp_u32(ws->id);
			gp_i32(cx);
			gp_i32(cy);
			gp_i32(cw);
			gp_i32(ch);
			gp_u32(w->context->fg);
			gp_end();
			posted = true;
		}
		if (process_events(-1) < 0)
			return -1;
		if (posted) {
			show(true);
			posted = false;
		}
	}
	(void)cx, (void)cy, (void)cw, (void)ch;
	return 1;
}

/* wgetevent: the next event of w, setting the event keywords
 * (rwindow.r). 0 ok; -1 window gone; -2 malformed queue. */
static int wgetevent(WBinding *w, Value *res) {
	Value xd, yd;
	if (wgetq(w, res) == -1)
		return -1;
	if (w->window->listp->n < 2)
		return -2;
	wgetq(w, &xd);
	wgetq(w, &yd);
	if (VKIND(xd) != KInt || VKIND(yd) != KInt)
		return -2;
	amperX = xd.w.i & 0xFFFF;
	if (amperX >= 0x8000)
		amperX -= 0x10000;
	amperY = yd.w.i & 0xFFFF;
	if (amperY >= 0x8000)
		amperY -= 0x10000;
	amperX -= w->context->dx;
	amperY -= w->context->dy;
	amperCol = 1 + XTOCOL(w, amperX);
	amperRow = YTOROW(w, amperY);
	xmod_control = xd.w.i & EQ_MOD_CONTROL;
	xmod_meta = xd.w.i & EQ_MOD_META;
	xmod_shift = xd.w.i & EQ_MOD_SHIFT;
	uint64_t i = (((uint64_t)yd.w.i) >> 16) & 0xFFF;
	i <<= 4 * ((((uint64_t)yd.w.i) >> 28) & 0x7);
	amperInterval = (int64_t)i;
	return 0;
}

/* ---- Reading from a window (rwindow.r's wgetchne, wgetche,
 * wgetstrg, wlongread) ---- */

/* wgetchne waits for a keystroke of w, dropping other events: 1, or
 * wgetevent's -1 (window gone) or -2 (malformed queue). */
static int wgetchne(WBinding *w, Value *res) {
	for (;;) {
		int i = wgetevent(w, res);
		if (i != 0)
			return i;
		if (VKIND(*res) == KString && VN(*res) > 0)
			return 1;
	}
}

/* wgetche is wgetchne, echoing the keystroke if echo is on (a return
 * as return and newline). */
static int wgetche(WBinding *w, Value *res) {
	int i = wgetchne(w, res);
	if (i != 1)
		return i;
	unsigned char c = (unsigned char)*vstr(*res).s;
	if (c <= 127 && ISECHOON(w)) {
		wputc(w, (char)c);
		if (c == '\r')
			wputc(w, '\n');
	}
	return 1;
}

/* gfx_read reads from window f, with echo: a line (k < 0: up to a
 * return or newline, as read()), or k characters (as reads()); a
 * backspace or delete takes back the last character. */
bool gfx_read(File *f, int64_t k, String *out) {
	WBinding *w = f->gfx;
	Buf b = {0};
	while (k < 0 || (int64_t)b.n < k) {
		Value e;
		int i = wgetche(w, &e);
		if (i == -1)
			runerr(141);
		if (i < 0)
			runerr(143);
		char c = *vstr(e).s;
		if (c == '\177' || c == '\010') {
			if (b.n > 0)
				b.n--;
			continue;
		}
		if (k < 0 && (c == '\r' || c == '\n'))
			break;
		bufputc(&b, c);
	}
	*out = strcopy(b.b ? b.b : "", (int64_t)b.n);
	free(b.b);
	return true;
}

/* ---- Windows: opening, mapping, geometry (rsdlwin.ri) ---- */

static int plstate(int iconic) {
	switch (iconic) {
	case HiddenState: return PL_HIDDEN;
	case IconicState: return PL_ICONIC;
	case MaximizedState: return PL_MAXIMAL;
	}
	return PL_NORMAL;
}

/* wmap makes the canvas and, unless it is hidden, its window. */
static int wmap(WBinding *w) {
	WState *ws = w->window;
	WContext *wc = w->context;
	if (!gp_connect())
		return Failed;
	uint32_t id = ++lastcanvas;
	const char *t = ws->windowlabel ? ws->windowlabel : "";
	gp_begin(P_CANVAS);
	gp_u32(id);
	gp_i32(ws->posx == -MaxInt ? NOPOS : ws->posx);
	gp_i32(ws->posy == -MaxInt ? NOPOS : ws->posy);
	gp_i32(ws->width);
	gp_i32(ws->height);
	gp_u32(wc->bg);
	gp_i32(plstate(ws->iconic));
	gp_u32(ISRESIZABLE(w) != 0);
	gp_str(t, strlen(t));
	gp_end();
	gp_reply(P_CANVAS);
	if (!gr_i32())
		return Failed;
	ws->id = id;
	ws->pixwidth = ws->width;
	ws->pixheight = ws->height;
	if (ws->iconic != HiddenState) {
		ws->win = true;
		nwins++;
		ws->bits |= 256; /* exposed */
	}
	if (ws->initpx) { /* image= */
		putpixels(w, 0, 0, ws->initw, ws->inith, ws->initpx, ws->initop);
		free(ws->initpx);
		free(ws->initop);
		ws->initpx = NULL;
		ws->initop = NULL;
	}
	sendicon(ws);
	return Succeeded;
}

static int setwidth(WBinding *w, int width) {
	if (width < 0)
		return Failed;
	w->window->width = width;
	return Succeeded;
}

static int setheight(WBinding *w, int height) {
	if (height < 0)
		return Failed;
	w->window->height = height;
	return Succeeded;
}

static int parsegeometry(const char *buf, int *x, int *y, int *width, int *height) {
	int retval = 0;
	if (isdigit((unsigned char)*buf)) {
		retval++;
		if ((*width = atoi(buf)) <= 0)
			return 0;
		while (isdigit((unsigned char)*++buf))
			;
		if (*buf++ != 'x')
			return 0;
		if ((*height = atoi(buf)) <= 0)
			return 0;
		while (isdigit((unsigned char)*++buf))
			;
	}
	if (*buf == '+' || *buf == '-') {
		retval += 2;
		*x = atoi(buf);
		buf++;
		while (isdigit((unsigned char)*buf))
			buf++;
		if (*buf != '+' && *buf != '-')
			return 0;
		*y = atoi(buf);
		buf++;
		while (isdigit((unsigned char)*buf))
			buf++;
		if (*buf)
			return 0;
	}
	return retval;
}

static int setgeometry(WBinding *w, const char *geo) {
	WState *ws = w->window;
	int width = 0, height = 0, x = 0, y = 0, status;
	if ((status = parsegeometry(geo, &x, &y, &width, &height)) == 0)
		return Error;
	if (status & 1) {
		ws->width = width;
		ws->height = height;
	}
	if ((ws->win || !ws->id) && (status & 2)) {
		ws->posx = x;
		ws->posy = y;
	}
	return Succeeded;
}

/* posnum parses setpos's and setsize's "a,b" numbers. */
static bool posnum(const char **sp, int *v) {
	const char *s = *sp;
	if (!isdigit((unsigned char)*s) && *s != '-')
		return false;
	*v = atoi(s);
	if (*s == '-')
		s++;
	while (isdigit((unsigned char)*s))
		s++;
	if (*s == '.') {
		s++;
		while (isdigit((unsigned char)*s))
			s++;
	}
	*sp = s;
	return true;
}

static int setpos(WBinding *w, const char *s) {
	char tmp[64];
	int posx, posy;
	while (isspace((unsigned char)*s))
		s++;
	if (!posnum(&s, &posx) || *s++ != ',' || !posnum(&s, &posy) || *s)
		return Error;
	snprintf(tmp, sizeof tmp, "%s%d%s%d", posx < 0 ? "" : "+", posx, posy < 0 ? "" : "+", posy);
	return setgeometry(w, tmp);
}

static int setsize(WBinding *w, const char *s) {
	char tmp[64];
	int width, height;
	while (isspace((unsigned char)*s))
		s++;
	if (!posnum(&s, &width) || *s++ != ',' || !posnum(&s, &height) || *s)
		return Error;
	snprintf(tmp, sizeof tmp, "%dx%d", width, height);
	return setgeometry(w, tmp);
}

/* do_config applies a new size (and, for windows, position): the canvas
 * grows as needed, never shrinks, like X's backing pixmap. */
static int do_config(WBinding *w, int status) {
	WState *ws = w->window;
	if (ws->id) {
		do_resize(ws, ws->width, ws->height, w->context->bg);
		if (ws->width > ws->pixwidth)
			ws->pixwidth = ws->width;
		if (ws->height > ws->pixheight)
			ws->pixheight = ws->height;
	}
	if (ws->win) {
		int x = (status == 2 || ws->posx == -MaxInt) ? NOPOS : ws->posx;
		int y = (status == 2 || ws->posy == -MaxInt) ? NOPOS : ws->posy;
		gp_begin(P_GEOMETRY);
		gp_u32(ws->id);
		gp_i32(x);
		gp_i32(y);
		gp_i32(status == 1 ? 0 : ws->width);
		gp_i32(status == 1 ? 0 : ws->height);
		gp_end();
	}
	return Succeeded;
}

static int allowresize(WBinding *w, bool on) {
	if (on)
		w->window->bits |= 128;
	else
		w->window->bits &= ~128;
	if (w->window->win) {
		gp_begin(P_RESIZABLE);
		gp_u32(w->window->id);
		gp_u8(on);
		gp_end();
	}
	return Succeeded;
}

static int getpos(WBinding *w) {
	WState *ws = w->window;
	if (!ws->win)
		return Succeeded;
	gp_begin(P_POSITION);
	gp_u32(ws->id);
	gp_end();
	gp_reply(P_POSITION);
	int ok = gr_i32(), x = gr_i32(), y = gr_i32();
	if (ok)
		ws->posx = x, ws->posy = y;
	return Succeeded;
}

static int setcanvas(WBinding *w, const char *s) {
	WState *ws = w->window;
	int state;
	if (!strcmp(s, "hidden"))
		state = HiddenState;
	else if (!strcmp(s, "normal"))
		state = NormalState;
	else if (!strcmp(s, "iconic"))
		state = IconicState;
	else if (!strcmp(s, "maximal"))
		state = MaximizedState;
	else
		return Error;
	ws->iconic = state;
	if (!ws->id)
		return Succeeded; /* not yet mapped: wmap does it */
	if (state == HiddenState) {
		if (ws->win) {
			gp_begin(P_STATE);
			gp_u32(ws->id);
			gp_i32(PL_HIDDEN);
			gp_end();
		}
		return Succeeded;
	}
	if (!ws->win) {
		const char *t = ws->windowlabel ? ws->windowlabel : "";
		gp_begin(P_MAP);
		gp_u32(ws->id);
		gp_i32(ws->posx == -MaxInt ? NOPOS : ws->posx);
		gp_i32(ws->posy == -MaxInt ? NOPOS : ws->posy);
		gp_i32(plstate(state));
		gp_u8(ISRESIZABLE(w) != 0);
		gp_str(t, strlen(t));
		gp_end();
		gp_reply(P_MAP);
		if (!gr_i32())
			return Failed;
		ws->win = true;
		nwins++;
		ws->bits |= 256;
		sendicon(ws);
	} else {
		gp_begin(P_STATE);
		gp_u32(ws->id);
		gp_i32(plstate(state));
		gp_end();
	}
	return Succeeded;
}

static void getcanvas(WBinding *w, char *s) {
	switch (w->window->iconic) {
	case HiddenState: strcpy(s, "hidden"); break;
	case IconicState: strcpy(s, "iconic"); break;
	case MaximizedState: strcpy(s, "maximal"); break;
	default: strcpy(s, "normal"); break;
	}
}

static int setwindowlabel(WBinding *w, const char *val) {
	free(w->window->windowlabel);
	w->window->windowlabel = salloc(val);
	if (w->window->win) {
		gp_begin(P_TITLE);
		gp_u32(w->window->id);
		gp_str(val, strlen(val));
		gp_end();
	}
	return Succeeded;
}

static int setdrawop(WBinding *w, const char *val) {
	WContext *wc = w->context;
	if (!strcmp(val, "reverse")) {
		wc->bits |= 16;
		wc->drawop = DRAWOP_XOR;
		return Succeeded;
	}
	int op = si_s2i(drawops, NDRAWOPS, val);
	if (op == -1)
		return Error;
	wc->bits &= ~16;
	wc->drawop = op;
	return Succeeded;
}

static void getdrawop(WBinding *w, char *answer) {
	const char *s = ISXORREVERSE(w) ? "reverse" : si_i2s(drawops, NDRAWOPS, w->context->drawop);
	strcpy(answer, s ? s : "copy");
}

static int setlinestyle(WBinding *w, const char *s) {
	WContext *wc = w->context;
	if (!strcmp(s, "solid"))
		wc->linestyle = LS_SOLID;
	else if (!strcmp(s, "onoff") || !strcmp(s, "dashed"))
		wc->linestyle = LS_DASHED;
	else if (!strcmp(s, "doubledash") || !strcmp(s, "striped"))
		wc->linestyle = LS_STRIPED;
	else
		return Error;
	return Succeeded;
}

static int setfillstyle(WBinding *w, const char *s) {
	WContext *wc = w->context;
	if (!strcmp(s, "solid"))
		wc->fillstyle = FS_SOLID;
	else if (!strcmp(s, "masked") || !strcmp(s, "stippled") || !strcmp(s, "patterned"))
		wc->fillstyle = FS_STIPPLE;
	else if (!strcmp(s, "textured") || !strcmp(s, "opaquestippled") || !strcmp(s, "opaquepatterned"))
		wc->fillstyle = FS_OPAQUESTIPPLE;
	else
		return Error;
	return Succeeded;
}

static const StrInt cursorsyms[] = {
	{"X cursor", 10}, {"arrow", 0}, {"based arrow down", 8}, {"based arrow up", 8},
	{"boat", 0}, {"bogosity", 10}, {"bottom left corner", 18}, {"bottom right corner", 16},
	{"bottom side", 17}, {"bottom tee", 0}, {"box spiral", 2}, {"center ptr", 0},
	{"circle", 10}, {"clock", 2}, {"coffee mug", 2}, {"cross", 3}, {"cross reverse", 3},
	{"crosshair", 3}, {"diamond cross", 3}, {"dot", 0}, {"dotbox", 0}, {"double arrow", 8},
	{"draft large", 0}, {"draft small", 0}, {"draped box", 0}, {"exchange", 0},
	{"fleur", 9}, {"gobbler", 0}, {"gumby", 0}, {"hand1", 11}, {"hand2", 11}, {"heart", 0},
	{"icon", 0}, {"iron cross", 3}, {"left ptr", 0}, {"left side", 19}, {"left tee", 0},
	{"leftbutton", 0}, {"ll angle", 0}, {"lr angle", 0}, {"man", 0}, {"middlebutton", 0},
	{"mouse", 0}, {"num glyphs", 0}, {"pencil", 3}, {"pirate", 10}, {"plus", 3},
	{"question arrow", 0}, {"right ptr", 0}, {"right side", 15}, {"right tee", 0},
	{"rightbutton", 0}, {"rtl logo", 0}, {"sailboat", 0}, {"sb down arrow", 8},
	{"sb h double arrow", 7}, {"sb left arrow", 7}, {"sb right arrow", 7},
	{"sb up arrow", 8}, {"sb v double arrow", 8}, {"shuttle", 0}, {"sizing", 9},
	{"spider", 0}, {"spraycan", 0}, {"star", 0}, {"target", 3}, {"tcross", 3},
	{"top left arrow", 0}, {"top left corner", 12}, {"top right corner", 14},
	{"top side", 13}, {"top tee", 0}, {"trek", 0}, {"ul angle", 0}, {"umbrella", 0},
	{"ur angle", 0}, {"watch", 2}, {"xterm", 1},
};

static int setpointer(WBinding *w, const char *val) {
	WState *ws = w->window;
	int c = si_s2i(cursorsyms, COUNT(cursorsyms), val);
	if (c < 0)
		return Failed;
	free(ws->cursorname);
	ws->cursorname = salloc(val);
	if (ws->win) {
		gp_begin(P_CURSOR);
		gp_u32(ws->id);
		gp_i32(c);
		gp_end();
	}
	return Succeeded;
}

static void warpPointer(WBinding *w, int x, int y) {
	if (w->window->win) {
		gp_begin(P_WARP);
		gp_u32(w->window->id);
		gp_i32(x);
		gp_i32(y);
		gp_end();
	}
}

static void displaysize(WBinding *w, int *dw, int *dh) {
	gp_begin(P_DISPLAYSIZE);
	gp_u32(w->window->id);
	gp_end();
	gp_reply(P_DISPLAYSIZE);
	*dw = gr_i32();
	*dh = gr_i32();
}

/* ---- Patterns (rwindow.r's parsepattern; rsdlwin.ri's SetPattern) ---- */

static const int patbits[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xEF, 0xFF, 0xFE, 0xFF, 0xEF, 0xFF,
	0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA,
	0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x01, 0x00, 0x10, 0x00, 0x01, 0x00, 0x10, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x10, 0x10, 0xFF, 0x10, 0x10, 0x10, 0x10, 0x82, 0x44, 0x28, 0x10, 0x28, 0x44, 0x82, 0x01,
	0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0, 0x1B, 0x18, 0x81, 0xB1, 0x36, 0x06, 0x60, 0x63,
	0x02, 0x02, 0x05, 0xF8, 0x20, 0x20, 0x50, 0x8F, 0x03, 0x84, 0x48, 0x30, 0x03, 0x84, 0x48, 0x30,
};

static const StrInt patternsyms[] = {
	{"black", 0}, {"checkers", 12}, {"darkgray", 2}, {"diagonal", 8}, {"grains", 13},
	{"gray", 3}, {"grid", 10}, {"horizontal", 9}, {"lightgray", 4}, {"scales", 14},
	{"trellis", 11}, {"vertical", 7}, {"verydark", 1}, {"verylight", 5}, {"waves", 15},
	{"white", 6},
};

enum { MAXXOBJS = 256 };

static int parsepattern(const char *s, int len, int *width, int *nbits, int64_t *bits) {
	int64_t v;
	int i, j, hexdigits_per_row, maxbits = *nbits;
	if (sscanf(s, "%d,", width) != 1)
		return Error;
	if (*width < 1)
		return Failed;
	while (len > 0 && isdigit((unsigned char)*s))
		len--, s++;
	if (len <= 1 || *s != ',')
		return Error;
	len--, s++;
	if (*s == '#') {
		s++, len--;
		if (len == 0)
			return Error;
		hexdigits_per_row = *width / 4;
		if (*width % 4)
			hexdigits_per_row++;
		*nbits = len / hexdigits_per_row;
		if (len % hexdigits_per_row)
			(*nbits)++;
		if (*nbits > maxbits)
			return Failed;
		for (i = 0; i < *nbits; i++) {
			v = 0;
			for (j = 0; j < hexdigits_per_row; j++, len--, s++) {
				if (len == 0)
					break;
				v <<= 4;
				if (isdigit((unsigned char)*s))
					v += *s - '0';
				else if (*s >= 'a' && *s <= 'f')
					v += *s - 'a' + 10;
				else if (*s >= 'A' && *s <= 'F')
					v += *s - 'A' + 10;
				else
					return Error;
			}
			*bits++ = v;
		}
	} else {
		if (*width > 32)
			return Failed;
		v = 0;
		*nbits = 0;
		while (len > 0) {
			while (len > 0 && isdigit((unsigned char)*s)) {
				v = v * 10 + *s - '0';
				len--, s++;
			}
			(*nbits)++;
			if (*nbits > maxbits)
				return Failed;
			*bits++ = v;
			v = 0;
			if (len > 0) {
				if (*s == ',')
					len--, s++;
				else {
					errnum = 205;
					return Error;
				}
			}
		}
	}
	return Succeeded;
}

static int SetPatternBits(WBinding *w, int width, const int64_t *bits, int nbits) {
	WContext *wc = w->context;
	if (width <= 0 || nbits <= 0)
		return Failed;
	unsigned char *p = xmalloc((size_t)width * (size_t)nbits);
	for (int y = 0; y < nbits; y++)
		for (int x = 0; x < width; x++)
			p[y * width + x] = x < 64 ? (bits[y] >> x) & 1 : 0;
	free(wc->pat);
	wc->pat = p;
	wc->patw = width;
	wc->path = nbits;
	return Succeeded;
}

static int SetPattern(WBinding *w, const char *name, int len) {
	WContext *wc = w->context;
	int width, nbits, symbol;
	int64_t bits[MAXXOBJS];
	free(wc->patternname);
	wc->patternname = xmalloc((size_t)len + 1);
	memcpy(wc->patternname, name, (size_t)len);
	wc->patternname[len] = 0;
	if (len > 0 && isdigit((unsigned char)name[0])) {
		nbits = MAXXOBJS;
		switch (parsepattern(name, len, &width, &nbits, bits)) {
		case Failed: return Failed;
		case Error:
			if (errnum != 205)
				errnum = 145;
			return Error;
		}
		return SetPatternBits(w, width, bits, nbits);
	}
	if ((symbol = si_s2i(patternsyms, COUNT(patternsyms), wc->patternname)) >= 0) {
		for (int i = 0; i < 8; i++)
			bits[i] = patbits[symbol * 8 + i];
		return SetPatternBits(w, 8, bits, 8);
	}
	return Failed;
}

/* ---- Attributes (rwindow.r's wattrib) ---- */

enum {
	A_ASCENT, A_BG, A_CANVAS, A_CEOL, A_CLIPH, A_CLIPW, A_CLIPX, A_CLIPY, A_COL,
	A_COLUMNS, A_CURSOR, A_DEPTH, A_DESCENT, A_DISPLAY, A_DISPLAYHEIGHT,
	A_DISPLAYWIDTH, A_DRAWOP, A_DX, A_DY, A_ECHO, A_FG, A_FHEIGHT, A_FILLSTYLE,
	A_FONT, A_FWIDTH, A_GAMMA, A_GEOMETRY, A_HEIGHT, A_ICONIC, A_ICONIMAGE,
	A_ICONLABEL, A_ICONPOS, A_IMAGE, A_LABEL, A_LEADING, A_LINES, A_LINESTYLE,
	A_LINEWIDTH, A_PATTERN, A_POINTER, A_POINTERCOL, A_POINTERROW, A_POINTERX,
	A_POINTERY, A_POS, A_POSX, A_POSY, A_RESIZE, A_REVERSE, A_ROW, A_ROWS, A_SIZE,
	A_VISUAL, A_WIDTH, A_WINDOWLABEL, A_X, A_Y,
};

static const StrInt attribs[] = {
	{"ascent", A_ASCENT}, {"bg", A_BG}, {"canvas", A_CANVAS}, {"ceol", A_CEOL},
	{"cliph", A_CLIPH}, {"clipw", A_CLIPW}, {"clipx", A_CLIPX}, {"clipy", A_CLIPY},
	{"col", A_COL}, {"columns", A_COLUMNS}, {"cursor", A_CURSOR}, {"depth", A_DEPTH},
	{"descent", A_DESCENT}, {"display", A_DISPLAY}, {"displayheight", A_DISPLAYHEIGHT},
	{"displaywidth", A_DISPLAYWIDTH}, {"drawop", A_DRAWOP}, {"dx", A_DX}, {"dy", A_DY},
	{"echo", A_ECHO}, {"fg", A_FG}, {"fheight", A_FHEIGHT}, {"fillstyle", A_FILLSTYLE},
	{"font", A_FONT}, {"fwidth", A_FWIDTH}, {"gamma", A_GAMMA}, {"geometry", A_GEOMETRY},
	{"height", A_HEIGHT}, {"iconic", A_ICONIC}, {"iconimage", A_ICONIMAGE},
	{"iconlabel", A_ICONLABEL}, {"iconpos", A_ICONPOS}, {"image", A_IMAGE},
	{"label", A_LABEL}, {"leading", A_LEADING}, {"lines", A_LINES},
	{"linestyle", A_LINESTYLE}, {"linewidth", A_LINEWIDTH}, {"pattern", A_PATTERN},
	{"pointer", A_POINTER}, {"pointercol", A_POINTERCOL}, {"pointerrow", A_POINTERROW},
	{"pointerx", A_POINTERX}, {"pointery", A_POINTERY}, {"pos", A_POS}, {"posx", A_POSX},
	{"posy", A_POSY}, {"resize", A_RESIZE}, {"reverse", A_REVERSE}, {"row", A_ROW},
	{"rows", A_ROWS}, {"size", A_SIZE}, {"visual", A_VISUAL}, {"width", A_WIDTH},
	{"windowlabel", A_WINDOWLABEL}, {"x", A_X}, {"y", A_Y},
};

/* cint converts an attribute value to an integer, as cnv:C_integer. */
static bool cint(const char *s, int64_t *v) {
	return cvint(cstrval((s)), v);
}

#define AttemptAttr(op) if ((op) != Succeeded) return Failed
#define ATOBOOL(s) ((s)[1] == 'n')

/* wattrib gets or sets one attribute ("name" or "name=value"), the
 * answer (for a get) in *answer. Error: errnum says which. */
static int wattrib(WBinding *w, const char *s, size_t len, Value *answer) {
	char val[256], abuf[256];
	int64_t tmp;
	WState *ws = w->window;
	WContext *wc = w->context;
	const char *mid = memchr(s, '=', len);

	if (pollevent() == -1)
		runerr(141);
	if (mid) {
		size_t la = (size_t)(mid - s), lv = len - la - 1;
		if (la >= sizeof abuf || lv >= sizeof val) {
			errnum = 145;
			return Error;
		}
		memcpy(abuf, s, la);
		abuf[la] = 0;
		memcpy(val, mid + 1, lv);
		val[lv] = 0;
		int a = si_s2i(attribs, COUNT(attribs), abuf);
		switch (a) {
		case A_LINES:
		case A_ROWS:
			if (!cint(val, &tmp) || tmp < 1)
				return Failed;
			if (setheight(w, (int)(ROWTOY(w, tmp) + MAXDESCENDER(w))) == Failed)
				return Failed;
			break;
		case A_COLUMNS:
			if (!cint(val, &tmp) || tmp < 1)
				return Failed;
			if (setwidth(w, (int)COLTOX(w, tmp + 1)) == Failed)
				return Failed;
			break;
		case A_HEIGHT:
			if (!cint(val, &tmp) || tmp < 1)
				return Failed;
			if (setheight(w, (int)tmp) == Failed)
				return Failed;
			break;
		case A_WIDTH:
			if (!cint(val, &tmp) || tmp < 1)
				return Failed;
			if (setwidth(w, (int)tmp) == Failed)
				return Failed;
			break;
		case A_SIZE: AttemptAttr(setsize(w, val)); break;
		case A_GEOMETRY: AttemptAttr(setgeometry(w, val)); break;
		case A_RESIZE:
			if (strcmp(val, "on") && strcmp(val, "off"))
				return Failed;
			allowresize(w, ATOBOOL(val));
			break;
		case A_ROW:
			if (!cint(val, &tmp))
				return Failed;
			ws->y = (int)ROWTOY(w, tmp) + wc->dy;
			break;
		case A_COL:
			if (!cint(val, &tmp))
				return Failed;
			ws->x = (int)COLTOX(w, tmp) + wc->dx;
			break;
		case A_CANVAS:
		case A_ICONIC: AttemptAttr(setcanvas(w, val)); break;
		case A_ICONIMAGE: { /* an image file (rsdlwin.ri's seticonimage) */
			unsigned *px;
			unsigned char *op;
			int iw, ih;
			if (!val[0] || !loadimage(w, val, 0, &px, &op, &iw, &ih))
				return Failed;
			for (size_t i = 0; i < (size_t)iw * (size_t)ih; i++)
				px[i] = op[i] ? 0xFF000000u | px[i] : 0;
			free(op);
			free(ws->iconimage);
			ws->iconimage = salloc(val);
			free(ws->iconpx);
			ws->iconpx = px;
			ws->iconw = iw;
			ws->iconh = ih;
			sendicon(ws);
			break;
		}
		case A_ICONLABEL:
			free(ws->iconlabel);
			ws->iconlabel = salloc(val);
			break;
		case A_ICONPOS: return Failed;
		case A_LABEL:
		case A_WINDOWLABEL: AttemptAttr(setwindowlabel(w, val)); break;
		case A_CURSOR:
			if (strcmp(val, "on") && strcmp(val, "off"))
				return Failed;
			if (ATOBOOL(val))
				ws->bits |= 2;
			else
				ws->bits &= ~2;
			break;
		case A_FONT: AttemptAttr(setfont(w, val)); break;
		case A_PATTERN: AttemptAttr(SetPattern(w, val, (int)strlen(val))); break;
		case A_POS: AttemptAttr(setpos(w, val)); break;
		case A_POSX: {
			char t[300];
			snprintf(t, sizeof t, "%s,%d", val, ws->posy);
			AttemptAttr(setpos(w, t));
			break;
		}
		case A_POSY: {
			char t[300];
			snprintf(t, sizeof t, "%d,%s", ws->posx, val);
			AttemptAttr(setpos(w, t));
			break;
		}
		case A_FG:
			if (cint(val, &tmp) && tmp < 0) {
				if (isetcolor(w, tmp, true) != Succeeded)
					return Failed;
			} else if (setfg(w, val) != Succeeded)
				return Failed;
			break;
		case A_BG:
			if (cint(val, &tmp) && tmp < 0) {
				if (isetcolor(w, tmp, false) != Succeeded)
					return Failed;
			} else if (setbg(w, val) != Succeeded)
				return Failed;
			break;
		case A_GAMMA: {
			double g;
			char c;
			if (sscanf(val, "%lf%c", &g, &c) != 1 || g <= 0.0)
				return Failed;
			if (setgamma(w, g) != Succeeded)
				return Failed;
			break;
		}
		case A_FILLSTYLE: AttemptAttr(setfillstyle(w, val)); break;
		case A_LINESTYLE: AttemptAttr(setlinestyle(w, val)); break;
		case A_LINEWIDTH:
			if (!cint(val, &tmp))
				return Failed;
			if (tmp > 32767)
				return Failed;
			wc->linewidth = (int)tmp;
			break;
		case A_POINTER: AttemptAttr(setpointer(w, val)); break;
		case A_DRAWOP: AttemptAttr(setdrawop(w, val)); break;
		case A_DISPLAY: {
			/* one display, the server's: as Arizona's X11 opens the
			 * display named, display= may name it when a window is
			 * opened ("", or $DISPLAY, as WAttrib reports it, which
			 * vdialog passes on), and it cannot change once open */
			const char *d = getenv("DISPLAY");
			if (ws->id || (*val && !(d && !strcmp(val, d))))
				return Failed;
			break;
		}
		case A_X:
			if (!cint(val, &tmp))
				return Failed;
			ws->x = (int)tmp + wc->dx;
			break;
		case A_Y:
			if (!cint(val, &tmp))
				return Failed;
			ws->y = (int)tmp + wc->dy;
			break;
		case A_DX:
			if (!cint(val, &tmp))
				return Failed;
			wc->dx = (int)tmp;
			break;
		case A_DY:
			if (!cint(val, &tmp))
				return Failed;
			wc->dy = (int)tmp;
			break;
		case A_LEADING:
			if (!cint(val, &tmp))
				return Failed;
			wc->leading = (int)tmp;
			break;
		case A_IMAGE: { /* the window's size and initial contents */
			unsigned *px;
			unsigned char *op;
			int iw, ih;
			if (!loadimage(w, val, 0, &px, &op, &iw, &ih))
				return Failed;
			if (ws->id) { /* the canvas exists: resized, and drawn now */
				if (setwidth(w, iw) != Succeeded || setheight(w, ih) != Succeeded) {
					free(px);
					free(op);
					return Failed;
				}
				putpixels(w, 0, 0, iw, ih, px, op);
				free(px);
				free(op);
				break;
			}
			free(ws->initpx);
			free(ws->initop);
			ws->initpx = px;
			ws->initop = op;
			ws->initw = iw;
			ws->inith = ih;
			ws->width = iw;
			ws->height = ih;
			break;
		}
		case A_ECHO:
			if (strcmp(val, "on") && strcmp(val, "off"))
				return Failed;
			if (ATOBOOL(val))
				ws->bits |= 1024;
			else
				ws->bits &= ~1024;
			break;
		case A_CEOL:
			if (strcmp(val, "on") && strcmp(val, "off"))
				return Failed;
			if (ATOBOOL(val))
				ws->bits |= 512;
			else
				ws->bits &= ~512;
			break;
		case A_CLIPX:
		case A_CLIPY:
		case A_CLIPW:
		case A_CLIPH:
			if (!*val) {
				wc->clipx = wc->clipy = 0;
				wc->clipw = wc->cliph = -1;
			} else {
				if (!cint(val, &tmp))
					return Failed;
				if (wc->clipw < 0) {
					wc->clipx = wc->clipy = 0;
					wc->clipw = ws->width;
					wc->cliph = ws->height;
				}
				switch (a) {
				case A_CLIPX: wc->clipx = (int)tmp; break;
				case A_CLIPY: wc->clipy = (int)tmp; break;
				case A_CLIPW: wc->clipw = (int)tmp; break;
				case A_CLIPH: wc->cliph = (int)tmp; break;
				}
			}
			break;
		case A_REVERSE:
			if (strcmp(val, "on") && strcmp(val, "off"))
				return Failed;
			if ((!ATOBOOL(val) && ISREVERSE(w)) || (ATOBOOL(val) && !ISREVERSE(w))) {
				toggle_fgbg(w);
				wc->bits ^= 8;
			}
			break;
		case A_POINTERX:
			if (!cint(val, &tmp))
				return Failed;
			ws->pointerx = (int)tmp + wc->dx;
			warpPointer(w, ws->pointerx, ws->pointery);
			break;
		case A_POINTERY:
			if (!cint(val, &tmp))
				return Failed;
			ws->pointery = (int)tmp + wc->dy;
			warpPointer(w, ws->pointerx, ws->pointery);
			break;
		case A_POINTERCOL:
			if (!cint(val, &tmp))
				return Failed;
			ws->pointerx = (int)COLTOX(w, tmp) + wc->dx;
			warpPointer(w, ws->pointerx, ws->pointery);
			break;
		case A_POINTERROW:
			if (!cint(val, &tmp))
				return Failed;
			ws->pointery = (int)ROWTOY(w, tmp) + wc->dy;
			warpPointer(w, ws->pointerx, ws->pointery);
			break;
		case A_DEPTH:
		case A_DISPLAYHEIGHT:
		case A_DISPLAYWIDTH:
		case A_FHEIGHT:
		case A_FWIDTH:
		case A_ASCENT:
		case A_DESCENT:
		case A_VISUAL:
			errnum = 147;
			return Error;
		default:
			errnum = 145;
			return Error;
		}
		return Succeeded;
	}

	/* get an attribute */
	if (len >= sizeof abuf) {
		errnum = 145;
		return Error;
	}
	memcpy(abuf, s, len);
	abuf[len] = 0;
	int a = si_s2i(attribs, COUNT(attribs), abuf);
#define STR(x) (*answer = cstrval((x)))
#define INT(x) (*answer = intval(x))
	switch (a) {
	case A_IMAGE:
		errnum = 147;
		return Error;
	case A_VISUAL: STR("4,8,256"); break;
	case A_DEPTH: INT(24); break;
	case A_DISPLAY: STR(getenv("DISPLAY") ? getenv("DISPLAY") : ""); break; /* Arizona's X11: $DISPLAY */
	case A_ASCENT: INT(ASCENT(w)); break;
	case A_DESCENT: INT(DESCENT(w)); break;
	case A_FHEIGHT: INT(FHEIGHT(w)); break;
	case A_FWIDTH: INT(FWIDTH(w)); break;
	case A_ROW: INT(YTOROW(w, ws->y - wc->dy)); break;
	case A_COL: INT(1 + XTOCOL(w, ws->x - wc->dx)); break;
	case A_POINTERROW: INT(YTOROW(w, ws->pointery - wc->dy)); break;
	case A_POINTERCOL: INT(1 + XTOCOL(w, ws->pointerx - wc->dx)); break;
	case A_LINES:
	case A_ROWS: INT(YTOROW(w, ws->height - DESCENT(w))); break;
	case A_COLUMNS: INT(XTOCOL(w, ws->width)); break;
	case A_POS:
	case A_POSX:
	case A_POSY:
		if (getpos(w) == Failed)
			return Failed;
		if (a == A_POS) {
			snprintf(abuf, sizeof abuf, "%d,%d", ws->posx, ws->posy);
			STR(abuf);
		} else
			INT(a == A_POSX ? ws->posx : ws->posy);
		break;
	case A_FG: STR(wc->fgname); break;
	case A_BG: STR(wc->bgname); break;
	case A_GAMMA: *answer = realval(wc->gamma); break;
	case A_FILLSTYLE:
		STR(wc->fillstyle == FS_SOLID ? "solid" : wc->fillstyle == FS_STIPPLE ? "masked" : "textured");
		break;
	case A_LINESTYLE:
		STR(wc->linestyle == LS_SOLID ? "solid" : wc->linestyle == LS_DASHED ? "dashed" : "striped");
		break;
	case A_LINEWIDTH: INT(wc->linewidth); break;
	case A_HEIGHT: INT(ws->height); break;
	case A_WIDTH: INT(ws->width); break;
	case A_SIZE:
		snprintf(abuf, sizeof abuf, "%d,%d", ws->width, ws->height);
		STR(abuf);
		break;
	case A_RESIZE: STR(ISRESIZABLE(w) ? "on" : "off"); break;
	case A_DISPLAYHEIGHT:
	case A_DISPLAYWIDTH: {
		int dw, dh;
		displaysize(w, &dw, &dh);
		INT(a == A_DISPLAYWIDTH ? dw : dh);
		break;
	}
	case A_CURSOR: STR(ISCURSORON(w) ? "on" : "off"); break;
	case A_ECHO: STR(ISECHOON(w) ? "on" : "off"); break;
	case A_CEOL: STR(ISCEOLON(w) ? "on" : "off"); break;
	case A_REVERSE: STR(ISREVERSE(w) ? "on" : "off"); break;
	case A_FONT: STR(wc->font->name); break;
	case A_X: INT(ws->x - wc->dx); break;
	case A_Y: INT(ws->y - wc->dy); break;
	case A_DX: INT(wc->dx); break;
	case A_DY: INT(wc->dy); break;
	case A_LEADING: INT(LEADING(w)); break;
	case A_POINTERX: INT(ws->pointerx - wc->dx); break;
	case A_POINTERY: INT(ws->pointery - wc->dy); break;
	case A_POINTER: STR(ws->cursorname ? ws->cursorname : "left ptr"); break;
	case A_DRAWOP:
		getdrawop(w, abuf);
		STR(abuf);
		break;
	case A_GEOMETRY:
		if (getpos(w) == Failed)
			return Failed;
		if (ws->win)
			snprintf(abuf, sizeof abuf, "%dx%d+%d+%d", ws->width, ws->height, ws->posx, ws->posy);
		else
			snprintf(abuf, sizeof abuf, "%dx%d", ws->pixwidth, ws->pixheight);
		STR(abuf);
		break;
	case A_CANVAS:
		getcanvas(w, abuf);
		STR(abuf);
		break;
	case A_ICONIC:
		STR(ws->iconic == IconicState ? "1" : "0");
		break;
	case A_ICONIMAGE: STR(ws->iconimage ? ws->iconimage : ""); break;
	case A_ICONLABEL:
		if (!ws->iconlabel)
			return Failed;
		STR(ws->iconlabel);
		break;
	case A_LABEL:
	case A_WINDOWLABEL:
		if (!ws->windowlabel)
			return Failed;
		STR(ws->windowlabel);
		break;
	case A_ICONPOS: return Failed;
	case A_PATTERN: STR(wc->patternname ? wc->patternname : "black"); break;
	case A_CLIPX: *answer = wc->clipw >= 0 ? intval(wc->clipx) : NULLV; break;
	case A_CLIPY: *answer = wc->clipw >= 0 ? intval(wc->clipy) : NULLV; break;
	case A_CLIPW: *answer = wc->clipw >= 0 ? intval(wc->clipw) : NULLV; break;
	case A_CLIPH: *answer = wc->clipw >= 0 ? intval(wc->cliph) : NULLV; break;
	default:
		errnum = 145;
		return Error;
	}
#undef STR
#undef INT
	wflush();
	return Succeeded;
}

/* ---- Window values ---- */

static WBinding *bindingof(WState *ws) { return ws->filep ? ws->filep->gfx : NULL; }

static Value winval(WBinding *w, String name) {
	return ptrval(KFile, file_window(name, w));
}

/* optwindow: the window argument a[0] if there is one (*warg 1), else
 * &window (OptWindow). */
static WBinding *optwindow(Value *a, int n, int *warg) {
	*warg = 0;
	Value v;
	if (n > 0 && VKIND(a[0]) == KFile) {
		v = a[0];
		*warg = 1;
	} else
		v = kwindow;
	if (VKIND(v) != KFile || !((File *)v.w.p)->gfx)
		runerrv(140, v);
	File *f = v.w.p;
	WBinding *w = f->gfx;
	if (f->closed || ISCLOSED(w))
		runerrv(142, v);
	return w;
}

static Value retwindow(Value *a, int warg) { return warg ? a[0] : kwindow; }

static int cshort(Value v) {
	int64_t i;
	if (!cvint(v, &i))
		runerrv(101, v);
	return (int)i;
}

/* defint: v, or d if it is null (def:C_integer); false if v is not an
 * integer. */
static bool defint(Value v, int64_t d, int64_t *out) {
	if (VKIND(v) == KNull) {
		*out = d;
		return true;
	}
	return cvint(v, out);
}

/* rectargs reads x, y, width, height from a[i...], with iconx's
 * defaults; the index of a bad argument, or -1. */
static int rectargs(WBinding *w, int argc, Value *a, int i, int *px, int *py, int *pw, int *ph) {
	WContext *wc = w->context;
	WState *ws = w->window;
	int64_t v;
	if (i >= argc)
		v = -wc->dx;
	else if (!defint(a[i], -wc->dx, &v))
		return i;
	*px = (int)v;
	if (++i >= argc)
		v = -wc->dy;
	else if (!defint(a[i], -wc->dy, &v))
		return i;
	*py = (int)v;
	*px += wc->dx;
	*py += wc->dy;
	int defw = ws->width - *px, defh = ws->height - *py;
	if (++i >= argc)
		v = defw;
	else if (!defint(a[i], defw, &v))
		return i;
	*pw = (int)v;
	if (++i >= argc)
		v = defh;
	else if (!defint(a[i], defh, &v))
		return i;
	*ph = (int)v;
	if (*pw < 0)
		*px -= (*pw = -*pw);
	if (*ph < 0)
		*py -= (*ph = -*ph);
	return -1;
}

static int multiple(int argc, int warg, int k) {
	if ((argc - warg) % k)
		runerr(146);
	int n = (argc - warg) / k;
	if (!n)
		runerr(146);
	return n;
}

/* ---- Hooks ---- */

File *gfx_open(String name, Value *attrs, int n) {
	for (int j = 0; j < n; j++) {
		if (VKIND(attrs[j]) == KNull)
			attrs[j] = cstrval((""));
		if (VKIND(attrs[j]) != KString)
			runerrv(109, attrs[j]);
	}
	if (!gp_connect())
		return NULL;
	WBinding *w = xcalloc(1, sizeof *w);
	w->window = alc_winstate();
	WState *ws = w->window;
	ws->listp = newlist(0);
	w->context = alc_context(w);
	if (!w->context) {
		free_window(ws);
		return NULL;
	}
	ws->height = FHEIGHT(w) * 12;
	ws->width = FWIDTH(w) * 80;
	ws->y = ASCENT(w) + w->context->dy;
	ws->x = w->context->dx;
	for (int i = 0; i < n; i++) {
		String s = vstr(attrs[i]);
		Value ans;
		switch (wattrib(w, s.s, (size_t)s.n, &ans)) {
		case Error:
			runerrv(145, attrs[i]);
		case Failed:
			free_window(ws);
			return NULL;
		}
	}
	if (!ws->windowlabel)
		ws->windowlabel = cstr(name) ? strdup(cstr(name)) : salloc("");
	if (wmap(w) != Succeeded) {
		free_window(ws);
		return NULL;
	}
	File *f = file_window(name, w);
	opened(f);
	return f;
}

void gfx_close(File *f) {
	WBinding *w = f->gfx;
	WState *ws = w->window;
	ws->bits |= 1;
	if (ws->win && ws->refcount > 1) {
		/* other bindings keep the canvas; the window goes */
		gp_begin(P_STATE);
		gp_u32(ws->id);
		gp_i32(PL_HIDDEN);
		gp_end();
		unmap(ws);
		ws->refcount--;
	} else
		free_window(ws);
	gp_flush();
}

void gfx_image(Buf *b, File *f) {
	WBinding *w = f->gfx;
	bufprintf(b, "window_%d:%d(", w->window->serial, w->context->serial);
	bufputs(b, w->window->windowlabel ? w->window->windowlabel : "");
	bufputc(b, ')');
}

void gfx_mark(void) {
	markvalue(kwindow);
	markvalue(lastEventWin);
	for (WState *ws = wstates; ws; ws = ws->next) {
		if (ws->listp)
			markvalue(ptrval(KList, ws->listp));
		if (ws->filep)
			markptr(ws->filep);
	}
}

void gfx_exit(void) { gp_close(); }

/* ---- Functions (fwindow.r) ---- */

FN(f_Fg) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (n - warg > 0) {
		if (VKIND(a[warg]) == KInt) {
			if (isetcolor(w, a[warg].w.i, true) == Failed)
				return false;
		} else if (setfg(w, cstr(muststr(a[warg]))) == Failed)
			return false;
	}
	*out = cstrval((w->context->fgname));
	return true;
}

FN(f_Bg) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (n - warg > 0) {
		if (VKIND(a[warg]) == KInt) {
			if (isetcolor(w, a[warg].w.i, false) == Failed)
				return false;
		} else if (setbg(w, cstr(muststr(a[warg]))) == Failed)
			return false;
	}
	*out = cstrval((w->context->bgname));
	return true;
}

/* points: k-int groups from a[warg...] with dx, dy added */
static int *points(WBinding *w, Value *a, int warg, int count, int k) {
	int *xy = xmalloc((size_t)count * (size_t)k * sizeof *xy + 1);
	for (int i = 0; i < count * k; i++)
		xy[i] = cshort(a[warg + i]) + (i % 2 ? w->context->dy : w->context->dx);
	return xy;
}

FN(f_DrawLine) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 2);
	int *xy = points(w, a, warg, c, 2);
	drawxy(w, P_LINES, xy, c, 2);
	free(xy);
	*out = retwindow(a, warg);
	return true;
}

FN(f_DrawPoint) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 2);
	int *xy = points(w, a, warg, c, 2);
	drawxy(w, P_POINTS, xy, c, 2);
	free(xy);
	*out = retwindow(a, warg);
	return true;
}

FN(f_DrawSegment) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 4);
	int *xy = points(w, a, warg, c, 4);
	drawxy(w, P_SEGMENTS, xy, c, 4);
	free(xy);
	*out = retwindow(a, warg);
	return true;
}

/* DrawPolygon: a closed polyline, starting from the last point. */
FN(f_DrawPolygon) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 2);
	int *xy = xmalloc((size_t)(c + 1) * 2 * sizeof *xy);
	xy[0] = cshort(a[n - 2]) + w->context->dx;
	xy[1] = cshort(a[n - 1]) + w->context->dy;
	int *p = points(w, a, warg, c, 2);
	memcpy(xy + 2, p, (size_t)c * 2 * sizeof *xy);
	free(p);
	drawxy(w, P_LINES, xy, c + 1, 2);
	free(xy);
	*out = retwindow(a, warg);
	return true;
}

FN(f_FillPolygon) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 2);
	int *xy = points(w, a, warg, c, 2);
	drawxy(w, P_FILLPOLY, xy, c, 2);
	free(xy);
	*out = retwindow(a, warg);
	return true;
}

static bool rects(Builtin *b, Value *a, int n, Value *out, int op) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int cap = (n - warg) / 4 + 1, j = 0;
	int *r = xmalloc((size_t)cap * 4 * sizeof *r);
	for (int i = warg; i < n || i == warg; i += 4) {
		int x, y, wd, ht, bad = rectargs(w, n, a, i, &x, &y, &wd, &ht);
		if (bad >= 0) {
			free(r);
			runerrv(101, a[bad]);
		}
		r[4 * j] = x, r[4 * j + 1] = y, r[4 * j + 2] = wd, r[4 * j + 3] = ht;
		j++;
	}
	drawxy(w, op, r, j, 4);
	free(r);
	*out = retwindow(a, warg);
	return true;
}

FN(f_DrawRectangle) { return rects(b, a, n, out, P_RECTS); }
FN(f_FillRectangle) { return rects(b, a, n, out, P_FILLRECTS); }

FN(f_EraseArea) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	for (int i = warg; i < n || i == warg; i += 4) {
		int x, y, wd, ht, bad = rectargs(w, n, a, i, &x, &y, &wd, &ht);
		if (bad >= 0)
			runerrv(101, a[bad]);
		eraseArea(w, x, y, wd, ht);
	}
	*out = retwindow(a, warg);
	return true;
}

static double cdouble(Value v, int err) {
	double d;
	if (!cvreal(v, &d))
		runerrv(err, v);
	return d;
}

static bool arcs(Value *a, int n, Value *out, bool fill) {
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int cap = (n - warg) / 6 + 1, j = 0;
	XArc *arcs = xmalloc((size_t)cap * sizeof *arcs);
	for (int i = warg; i < n || i == warg; i += 6) {
		int x, y, wd, ht, bad = rectargs(w, n, a, i, &x, &y, &wd, &ht);
		double a1, a2;
		if (bad >= 0) {
			free(arcs);
			runerrv(101, a[bad]);
		}
		arcs[j].x = x, arcs[j].y = y, arcs[j].width = wd, arcs[j].height = ht;
		if (i + 4 >= n || VKIND(a[i + 4]) == KNull)
			a1 = 0.0;
		else {
			a1 = cdouble(a[i + 4], 102);
			a1 = a1 >= 0.0 ? fmod(a1, 2 * Pi) : -fmod(-a1, 2 * Pi);
		}
		if (i + 5 >= n || VKIND(a[i + 5]) == KNull)
			a2 = 2 * Pi;
		else {
			a2 = cdouble(a[i + 5], 102);
			if (fabs(a2) > 3 * Pi)
				runerrv(101, a[i + 5]);
		}
		if (fabs(a2) >= 2 * Pi)
			a2 = 2 * Pi;
		else if (a2 < 0.0) {
			a1 += a2;
			a2 = fabs(a2);
		}
		if (a1 < 0.0)
			a1 = 2 * Pi - fmod(fabs(a1), 2 * Pi);
		else
			a1 = fmod(a1, 2 * Pi);
		arcs[j].angle1 = a1;
		arcs[j].angle2 = a2;
		j++;
	}
	drawarcs(w, arcs, j, fill);
	free(arcs);
	*out = retwindow(a, warg);
	return true;
}

FN(f_DrawArc) { (void)b; return arcs(a, n, out, false); }
FN(f_FillArc) { (void)b; return arcs(a, n, out, true); }

/* docircles (rwindow.r): x, y, r, theta, alpha groups. */
static int docircles(WBinding *w, int argc, Value *argv, bool fill) {
	int dx = w->context->dx, dy = w->context->dy;
	for (int i = 0; i < argc; i += 5) {
		double x, y, r, theta, alpha;
		if (i + 2 >= argc)
			return i + 2;
		if (!cvreal(argv[i], &x))
			return i;
		if (!cvreal(argv[i + 1], &y))
			return i + 1;
		if (!cvreal(argv[i + 2], &r))
			return i + 2;
		if (i + 3 >= argc || VKIND(argv[i + 3]) == KNull)
			theta = 0.0;
		else if (!cvreal(argv[i + 3], &theta))
			return i + 3;
		if (i + 4 >= argc || VKIND(argv[i + 4]) == KNull)
			alpha = 2 * Pi;
		else if (!cvreal(argv[i + 4], &alpha))
			return i + 4;
		if (r < 0) {
			r = -r;
			theta += Pi;
		}
		if (alpha < 0) {
			theta += alpha;
			alpha = -alpha;
		}
		theta = fmod(theta, 2 * Pi);
		if (theta > 0)
			theta -= 2 * Pi;
		XArc arc;
		arc.x = (int)(x + dx - r);
		arc.y = (int)(y + dy - r);
		arc.width = (int)(2 * r);
		arc.height = (int)(2 * r);
		arc.angle1 = theta;
		arc.angle2 = alpha >= 2 * Pi ? 2 * Pi : alpha;
		drawarcs(w, &arc, 1, fill);
	}
	return -1;
}

static bool circles(Value *a, int n, Value *out, bool fill) {
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int r = docircles(w, n - warg, a + warg, fill);
	if (r >= 0) {
		r += warg;
		if (r < n)
			runerrv(102, a[r]);
		runerr(146);
	}
	*out = retwindow(a, warg);
	return true;
}

FN(f_DrawCircle) { (void)b; return circles(a, n, out, false); }
FN(f_FillCircle) { (void)b; return circles(a, n, out, true); }

FN(f_DrawString) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 3);
	for (int i = 0; i < c; i++) {
		int base = warg + i * 3;
		int x = cshort(a[base]) + w->context->dx;
		int y = cshort(a[base + 1]) + w->context->dy;
		String s = muststr(a[base + 2]);
		text(w, x, y, s.s, (size_t)s.n, false);
	}
	*out = retwindow(a, warg);
	return true;
}

FN(f_TextWidth) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (warg == n)
		runerrv(103, NULLV);
	String s = muststr(a[warg]);
	*out = intval(textWidth(w, s.s, (size_t)s.n));
	return true;
}

FN(f_Font) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (warg < n && setfont(w, cstr(muststr(a[warg]))) == Failed)
		return false;
	*out = cstrval((w->context->font->name));
	return true;
}

FN(f_CopyArea) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg), *w2 = w;
	if (n > warg && VKIND(a[warg]) == KFile) {
		File *f = a[warg].w.p;
		if (!f->gfx)
			runerrv(140, a[warg]);
		if (f->closed || ISCLOSED((WBinding *)f->gfx))
			runerrv(142, a[warg]);
		w2 = f->gfx;
		warg++;
	}
	int x, y, wd, ht, x2, y2, wd2, ht2;
	int r = rectargs(w, n, a, warg, &x, &y, &wd, &ht);
	if (r >= 0)
		runerrv(101, a[r]);
	int m = n > warg + 6 ? warg + 6 : n;
	r = rectargs(w2, m, a, warg + 4, &x2, &y2, &wd2, &ht2);
	if (r >= 0)
		runerrv(101, a[r]);
	if (copyArea(w, w2, x, y, wd, ht, x2, y2) == Failed)
		return false;
	*out = retwindow(a, warg > 1 ? 1 : warg);
	return true;
}

FN(f_Event) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	Value d = NULLV;
	int i = wgetevent(w, &d);
	if (i == 0) {
		if (VKIND(kwindow) == KFile && ((File *)kwindow.w.p)->gfx == w)
			lastEventWin = kwindow;
		else
			lastEventWin = warg ? a[0] : kwindow;
		*out = d;
		return true;
	}
	if (i == -1)
		runerr(141);
	runerr(143);
}

FN(f_Pending) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	WState *ws = w->window;
	wflush();
	for (int i = warg; i < n; i++)
		list_append(ws->listp, a[i]);
	if (pollevent() == -1)
		runerr(141);
	*out = ptrval(KList, ws->listp);
	return true;
}

FN(f_WFlush) {
	(void)b;
	int warg;
	optwindow(a, n, &warg);
	wflush();
	*out = retwindow(a, warg);
	return true;
}

FN(f_WSync) {
	(void)b;
	int warg;
	optwindow(a, n, &warg);
	wflush();
	pollevent();
	*out = retwindow(a, warg);
	return true;
}

FN(f_Raise) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (w->window->win) {
		gp_begin(P_RAISE);
		gp_u32(w->window->id);
		gp_end();
	}
	*out = retwindow(a, warg);
	return true;
}

FN(f_Lower) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (w->window->win) {
		gp_begin(P_LOWER);
		gp_u32(w->window->id);
		gp_end();
	}
	*out = retwindow(a, warg);
	return true;
}

FN(f_Alert) {
	(void)b;
	int warg;
	optwindow(a, n, &warg);
	int64_t v = 0;
	if (n > warg && !defint(a[warg], 0, &v))
		runerrv(101, a[warg]);
	gp_begin(P_BEEP);
	gp_i32((int32_t)v);
	gp_end();
	gp_flush();
	*out = retwindow(a, warg);
	return true;
}

FN(f_GotoXY) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int64_t x = 0, y = 0;
	if (n > warg && !defint(a[warg], 0, &x))
		runerrv(101, a[warg]);
	if (n > warg + 1 && !defint(a[warg + 1], 0, &y))
		runerrv(101, a[warg + 1]);
	w->window->x = (int)x + w->context->dx;
	w->window->y = (int)y + w->context->dy;
	*out = retwindow(a, warg);
	return true;
}

FN(f_GotoRC) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int64_t r = 1, c = 1;
	if (n > warg && !defint(a[warg], 1, &r))
		runerrv(101, a[warg]);
	if (n > warg + 1 && !defint(a[warg + 1], 1, &c))
		runerrv(101, a[warg + 1]);
	w->window->x = (int)COLTOX(w, c) + w->context->dx;
	w->window->y = (int)ROWTOY(w, r) + w->context->dy;
	*out = retwindow(a, warg);
	return true;
}

/* QueryPointer(w) generates the pointer's x and y, as iconx's: for a
 * window, where the pointer was at its last event (query_pointer); for
 * null, on the screen (query_rootpointer). */
typedef struct PairGen {
	Gen g;
	int64_t v[2];
	int i;
} PairGen;

static void pairmark(Gen *g) { (void)g; }

static bool pairnext(Gen *gg, Value *out) {
	PairGen *g = (PairGen *)gg;
	if (g->i >= 2)
		return false;
	*out = intval(g->v[g->i++]);
	return true;
}

GENFN(f_QueryPointer) {
	(void)b;
	PairGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = pairnext;
	g->g.mark = pairmark;
	pollevent();
	Value w = n > 0 ? a[0] : NULLV;
	if (VKIND(w) == KNull) {
		if (!gp_connect()) {
			g->v[0] = g->v[1] = 0;
			return &g->g;
		}
		gp_begin(P_GLOBALMOUSE);
		gp_end();
		gp_reply(P_GLOBALMOUSE);
		int ok = gr_i32(), x = gr_i32(), y = gr_i32();
		g->v[0] = ok ? x : 0;
		g->v[1] = ok ? y : 0;
	} else {
		if (VKIND(w) != KFile || !((File *)w.w.p)->gfx)
			runerrv(140, w);
		WState *ws = ((WBinding *)((File *)w.w.p)->gfx)->window;
		g->v[0] = ws->pointerx;
		g->v[1] = ws->pointery;
	}
	return &g->g;
}

FN(f_Clone) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	WBinding *w2 = xcalloc(1, sizeof *w2);
	w2->window = w->window;
	w2->window->refcount++;
	if (n > warg && VKIND(a[warg]) == KFile) {
		File *f = a[warg].w.p;
		if (!f->gfx)
			runerrv(140, a[warg]);
		if (f->closed || ISCLOSED((WBinding *)f->gfx))
			runerrv(142, a[warg]);
		w2->context = clone_context(f->gfx);
		warg++;
	} else
		w2->context = clone_context(w);
	if (!w2->context)
		runerr(305);
	for (int i = warg; i < n; i++) {
		if (VKIND(a[i]) == KNull)
			continue;
		String s = muststr(a[i]);
		Value ans;
		switch (wattrib(w2, s.s, (size_t)s.n, &ans)) {
		case Failed: return false;
		case Error: runerr(errnum); /* iconx: runerr(0, ...), no value */
		}
	}
	*out = winval(w2, textc(""));
	return true;
}

FN(f_Couple) {
	(void)b;
	Value x = ARG(0), y = ARG(1);
	if (VKIND(x) != KFile || !((File *)x.w.p)->gfx)
		runerrv(140, x);
	if (VKIND(y) != KFile || !((File *)y.w.p)->gfx)
		runerrv(140, y);
	WBinding *wb = ((File *)x.w.p)->gfx, *w2 = ((File *)y.w.p)->gfx;
	WBinding *nw = xcalloc(1, sizeof *nw);
	nw->window = wb->window;
	nw->context = w2->context;
	nw->context->refcount++;
	nw->window->refcount++;
	*out = winval(nw, textc(""));
	return true;
}

FN(f_Uncouple) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	File *f = (warg ? a[0] : kwindow).w.p;
	/* As iconx's (free_binding): the value is no longer open, and the
	 * canvas goes only when its last binding does; close() would
	 * close the window for every binding. */
	f->closed = true;
	free_window(w->window);
	*out = retwindow(a, warg);
	return true;
}

/* Clip(w, x, y, width, height): no arguments turn clipping off. */
FN(f_Clip) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	WContext *wc = w->context;
	if (n - warg <= 0) {
		wc->clipx = wc->clipy = 0;
		wc->clipw = wc->cliph = -1;
	} else {
		int x, y, wd, ht, r = rectargs(w, n, a, warg, &x, &y, &wd, &ht);
		if (r >= 0)
			runerrv(101, a[r]);
		wc->clipx = x, wc->clipy = y, wc->clipw = wd, wc->cliph = ht;
	}
	*out = retwindow(a, warg);
	return true;
}

/* WAttrib: assignments first, then the queries, as results
 * (fwindow.r). */
typedef struct WAttribGen {
	Gen g;
	Value *args;
	int n, i, warg;
	WBinding *w;
} WAttribGen;

static bool wattribnext(Gen *gg, Value *out) {
	WAttribGen *g = (WAttribGen *)gg;
	while (g->i < g->n) {
		Value v = g->args[g->i++];
		if (VKIND(v) == KFile) {
			g->w = ((File *)v.w.p)->gfx;
			continue;
		}
		if (VKIND(v) == KNull)
			continue;
		String s = vstr(v);
		const char *eq = memchr(s.s, '=', (size_t)s.n);
		size_t len = eq ? (size_t)(eq - s.s) : (size_t)s.n;
		Value ans;
		switch (wattrib(g->w, s.s, len, &ans)) {
		case Failed: continue;
		case Error: runerr(errnum); /* iconx: runerr(0, ...), no value */
		}
		*out = ans;
		return true;
	}
	return false;
}

static void wattribmark(Gen *gg) {
	WAttribGen *g = (WAttribGen *)gg;
	markptr(g->args);
}

GENFN(f_WAttrib) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg), *wsave = w;
	int config = 0;
	Value *args = gcnew((size_t)(n > 0 ? n : 1) * sizeof(Value), T_VALUES);
	memcpy(args, a, (size_t)n * sizeof(Value));
	/* pass 1: the assignments */
	for (int i = warg; i < n; i++) {
		Value v = args[i];
		if (VKIND(v) == KFile) {
			if (!((File *)v.w.p)->gfx)
				runerrv(140, v);
			w = ((File *)v.w.p)->gfx;
			continue;
		}
		if (VKIND(v) == KNull)
			runerrv(109, v);
		if (VKIND(v) == KInt || VKIND(v) == KReal)
			runerrv(145, v);
		String s;
		if (!cvstr(v, &s))
			runerrv(109, v);
		if (s.n > 127)
			runerrv(145, v);
		args[i] = strval(s);
		if (!memchr(s.s, '=', (size_t)s.n))
			continue;
		Value ans;
		switch (wattrib(w, s.s, (size_t)s.n, &ans)) {
		case Failed:
			args[i] = NULLV;
			continue;
		case Error:
			runerr(errnum); /* iconx: runerr(0, ...), no value */
		}
		static const struct { const char *p; int c; } cfg[] = {
			{"pos=", 1}, {"posx=", 1}, {"posy=", 1}, {"rows=", 2}, {"size=", 2},
			{"width=", 2}, {"lines=", 2}, {"height=", 2}, {"resize=", 2},
			{"columns=", 2}, {"geometry=", 3},
		};
		for (int k = 0; k < (int)(sizeof cfg / sizeof cfg[0]); k++)
			if ((size_t)s.n > strlen(cfg[k].p) && !strncmp(s.s, cfg[k].p, strlen(cfg[k].p)))
				config |= cfg[k].c;
	}
	if (config) {
		/* as fwindow.r: each window named, and the first, reconfigured */
		if (do_config(wsave, config) == Failed)
			return &failgen;
		for (int i = warg; i < n; i++)
			if (VKIND(args[i]) == KFile)
				do_config(((File *)args[i].w.p)->gfx, config);
	}
	WAttribGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = wattribnext;
	g->g.mark = wattribmark;
	g->args = args;
	g->n = n;
	g->i = warg;
	g->w = wsave;
	return &g->g;
}

/* Pixel: the colours of a rectangle's pixels, row by row: "r,g,b"
 * strings, linear Icon colours (as getpixel in rsdlwin.ri). */
typedef struct PixelGen {
	Gen g;
	WBinding *w;
	int x, y, width, height, i, j;
	int ix, iy, iw, ih;
	unsigned *pix;
	Value last;
} PixelGen;

static bool pixelnext(Gen *gg, Value *out) {
	PixelGen *g = (PixelGen *)gg;
	if (g->j >= g->y + g->height) {
		free(g->pix);
		g->pix = NULL;
		return false;
	}
	int i = g->i, j = g->j;
	if (++g->i >= g->x + g->width) {
		g->i = g->x;
		g->j++;
	}
	unsigned c;
	if (!g->pix || i < g->ix || i >= g->ix + g->iw || j < g->iy || j >= g->iy + g->ih)
		c = g->w->context->bg;
	else
		c = g->pix[(j - g->iy) * g->iw + (i - g->ix)];
	long r, gr, bl;
	sdl_lcolor(g->w, c, &r, &gr, &bl);
	char s[64];
	snprintf(s, sizeof s, "%ld,%ld,%ld", r, gr, bl);
	if (VKIND(g->last) != KString || !streq_(vstr(g->last), s))
		g->last = cstrval((s));
	*out = g->last;
	return true;
}

static void pixelmark(Gen *gg) { markvalue(((PixelGen *)gg)->last); }

GENFN(f_Pixel) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int x, y, wd, ht, r = rectargs(w, n, a, warg, &x, &y, &wd, &ht);
	if (r >= 0)
		runerrv(101, a[r]);
	WState *ws = w->window;
	PixelGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = pixelnext;
	g->g.mark = pixelmark;
	g->w = w;
	g->x = x, g->y = y, g->width = wd, g->height = ht;
	g->i = x, g->j = y;
	g->last = NULLV;
	g->ix = x > 0 ? x : 0;
	g->iy = y > 0 ? y : 0;
	g->iw = wd < ws->width - g->ix ? wd : ws->width - g->ix;
	g->ih = ht < ws->height - g->iy ? ht : ws->height - g->iy;
	if (wd <= 0 || ht <= 0)
		return &failgen;
	if (g->iw > 0 && g->ih > 0 && ws->id) {
		gp_begin(P_READ);
		gp_u32(ws->id);
		gp_i32(g->ix);
		gp_i32(g->iy);
		gp_i32(g->iw);
		gp_i32(g->ih);
		gp_u32(w->context->bg);
		gp_end();
		gp_reply(P_READ);
		size_t k = (size_t)g->iw * (size_t)g->ih;
		g->pix = xmalloc(k * sizeof *g->pix);
		for (size_t q = 0; q < k; q++)
			g->pix[q] = gr_u32();
	}
	return &g->g;
}

FN(f_Pattern) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (n - warg == 0)
		runerrv(103, NULLV);
	String s = muststr(a[warg]);
	switch (SetPattern(w, s.s, (int)s.n)) {
	case Error: runerrv(errnum, a[warg]);
	case Failed: return false;
	}
	*out = retwindow(a, warg);
	return true;
}

/* ColorValue(w, s): s as "r,g,b", 16-bit linear; a window is optional,
 * and need not exist at all. */
FN(f_ColorValue) {
	(void)b;
	int warg = 0;
	WBinding *w = NULL;
	if (n > 0 && VKIND(a[0]) == KFile && ((File *)a[0].w.p)->gfx) {
		w = ((File *)a[0].w.p)->gfx;
		warg = 1;
	} else if (VKIND(kwindow) == KFile && ((File *)kwindow.w.p)->gfx)
		w = ((File *)kwindow.w.p)->gfx;
	if (warg >= n)
		runerr(103);
	if (VKIND(a[warg]) == KInt)
		return false; /* no mutable colours */
	long r, g, bl;
	WBinding tmp;
	WContext tc;
	if (!w) { /* parsecolor needs a gamma for X's names */
		memset(&tc, 0, sizeof tc);
		tc.gamma = sdl_gamma();
		tmp.context = &tc;
		w = &tmp;
	}
	if (parsecolor(w, cstr(muststr(a[warg])), &r, &g, &bl) != Succeeded)
		return false;
	char s[64];
	snprintf(s, sizeof s, "%ld,%ld,%ld", r, g, bl);
	*out = cstrval(s);
	return true;
}

/* WDefault: no defaults database (as icon-sdl's). */
FN(f_WDefault) {
	(void)b, (void)out;
	int warg;
	optwindow(a, n, &warg);
	if (n - warg < 2)
		runerr(103);
	muststr(a[warg]);
	muststr(a[warg + 1]);
	return false;
}

/* Active() is a window with an event queued, waiting for one if there
 * is none, the windows taken in turn (rwindow.r's getactivewindow);
 * it fails if no window is open. */
FN(f_Active) {
	(void)b, (void)a, (void)n;
	static long next = 0;
	long nwindows = 0;
	for (WState *ws = wstates; ws; ws = ws->next)
		nwindows++;
	if (nwindows == 0)
		return false;
	next %= nwindows;
	WState *ptr = wstates;
	for (long i = 0; i < next; i++)
		ptr = ptr->next;
	for (;;) {
		if (pollevent() < 0)
			runerr(141);
		WState *ws = ptr;
		long j = next + 1;
		for (long i = 0; i < nwindows; ws = ws->next ? ws->next : wstates, i++, j++)
			if (ws->filep && ws->listp->n > 0) {
				next = j;
				*out = ptrval(KFile, ws->filep);
				return true;
			}
		/* none: wait for events (iconx sleeps and polls) */
		if (nwins && process_events(50) < 0)
			runerr(141);
		if (!nwins)
			nanosleep(&(struct timespec){0, 50000000}, NULL);
	}
}

/* Mutable colours are not supported (as icon-sdl's). */
FN(f_FreeColor) {
	(void)b;
	int warg;
	optwindow(a, n, &warg);
	if (n - warg == 0)
		runerr(103);
	*out = retwindow(a, warg);
	return true;
}

FN(f_NewColor) {
	(void)b, (void)out;
	int warg;
	optwindow(a, n, &warg);
	return false;
}

FN(f_Color) {
	(void)b, (void)out;
	int warg;
	optwindow(a, n, &warg);
	if (n - warg == 0)
		runerr(101);
	return false;
}

/* ---- Keywords ---- */

/* The graphics keywords are KVar variables of kind VGraphics, k their
 * index: 0 &window, then the integers &x, &y, &row, &col, &interval. */
enum { KW_WINDOW, KW_X, KW_Y, KW_ROW, KW_COL, KW_INTERVAL, NKW };
static Var kwvars[NKW];
static int lastEvFWidth = 1, lastEvLeading = 1, lastEvAscent;

static int64_t *kwint(int k) {
	switch (k) {
	case KW_X: return &amperX;
	case KW_Y: return &amperY;
	case KW_ROW: return &amperRow;
	case KW_COL: return &amperCol;
	}
	return &amperInterval;
}

Value gfx_kwget(int k) { return k == KW_WINDOW ? kwindow : intval(*kwint(k)); }

/* lastwin: the binding of the last event's window, if still open. */
static WBinding *lastwin(void) {
	if (VKIND(lastEventWin) != KFile)
		return NULL;
	File *f = lastEventWin.w.p;
	return f->gfx && !f->closed ? f->gfx : NULL;
}

/* gfx_kwset assigns to a graphics keyword, with xyrowcol's side
 * effects (rwindow.r): &x and &col, &y and &row, follow each other. */
bool gfx_kwset(int k, Value x) {
	if (k == KW_WINDOW) {
		if (VKIND(x) != KNull && (VKIND(x) != KFile || !((File *)x.w.p)->gfx))
			runerrv(140, x);
		kwindow = x;
		return true;
	}
	int64_t i;
	if (!cvint(x, &i))
		runerrv(101, x);
	*kwint(k) = i;
	WBinding *w = lastwin();
	switch (k) {
	case KW_X:
		amperCol = w ? 1 + XTOCOL(w, amperX) : 1 + amperX / lastEvFWidth;
		break;
	case KW_Y:
		amperRow = w ? YTOROW(w, amperY) : amperY / lastEvLeading + 1;
		break;
	case KW_COL:
		amperX = w ? COLTOX(w, amperCol) : (amperCol - 1) * lastEvFWidth;
		break;
	case KW_ROW:
		amperY = w ? ROWTOY(w, amperRow) : (amperRow - 1) * lastEvLeading + lastEvAscent;
		break;
	}
	return true;
}

static void needevent(void) {
	if (VKIND(lastEventWin) == KNull)
		runerrv(140, lastEventWin);
}

static bool kwvar(int k, Value *out) {
	kwvars[k].kind = VGraphics;
	kwvars[k].k = intval(k);
	*out = ptrval(KVar, &kwvars[k]);
	return true;
}

FN(k_window) { (void)b, (void)a, (void)n; return kwvar(KW_WINDOW, out); }
FN(k_x) { (void)b, (void)a, (void)n; needevent(); return kwvar(KW_X, out); }
FN(k_y) { (void)b, (void)a, (void)n; needevent(); return kwvar(KW_Y, out); }
FN(k_row) { (void)b, (void)a, (void)n; needevent(); return kwvar(KW_ROW, out); }
FN(k_col) { (void)b, (void)a, (void)n; needevent(); return kwvar(KW_COL, out); }
FN(k_interval) { (void)b, (void)a, (void)n; needevent(); return kwvar(KW_INTERVAL, out); }

static bool modkey(bool down, Value *out) {
	needevent();
	if (!down)
		return false;
	*out = NULLV;
	return true;
}

FN(k_control) { (void)b, (void)a, (void)n; return modkey(xmod_control, out); }
FN(k_shift) { (void)b, (void)a, (void)n; return modkey(xmod_shift, out); }
FN(k_meta) { (void)b, (void)a, (void)n; return modkey(xmod_meta, out); }

/* gfx_opened: a window was opened as f (open sets the last event's
 * window if there is none, as fsys.r's). */
static void opened(File *f) {
	WBinding *w = f->gfx;
	w->window->filep = f;
	if (VKIND(lastEventWin) == KNull) {
		lastEventWin = ptrval(KFile, f);
		lastEvFWidth = FWIDTH(w) ? FWIDTH(w) : 1;
		lastEvLeading = LEADING(w) ? LEADING(w) : 1;
		lastEvAscent = ASCENT(w);
	}
}


/* ---- Images, palettes and curves (fwindow.r's DrawImage, Palette*,
 * ReadImage, WriteImage and DrawCurve; rsdlwin.ri's strimage, blimage,
 * capture, readimage and dumpimage; rwindow.r's genCurve) ---- */

/* putpixels writes pixels (0xRRGGBB) to w's canvas where op is
 * nonzero. */
static void putpixels(WBinding *w, int x, int y, int wd, int ht, const unsigned *px, const unsigned char *op) {
	if (!w->window->id || wd <= 0 || ht <= 0)
		return;
	uint32_t gc = wgc(w);
	size_t k = (size_t)wd * (size_t)ht;
	gp_begin(P_WRITE);
	gp_u32(w->window->id);
	gp_u32(gc);
	gp_i32(x);
	gp_i32(y);
	gp_i32(wd);
	gp_i32(ht);
	for (size_t i = 0; i < k; i++)
		gp_u32(px[i]);
	gp_bytes(op, k);
	gp_end();
	drawing();
}

/* getpixels reads w's canvas (0xRRGGBB; outside it, bg). */
static unsigned *getpixels(WBinding *w, int x, int y, int wd, int ht) {
	size_t k = (size_t)wd * (size_t)ht;
	unsigned *px = xmalloc(k * sizeof *px + 1);
	gp_begin(P_READ);
	gp_u32(w->window->id);
	gp_i32(x);
	gp_i32(y);
	gp_i32(wd);
	gp_i32(ht);
	gp_u32(w->context->bg);
	gp_end();
	gp_reply(P_READ);
	for (size_t i = 0; i < k; i++)
		px[i] = gr_u32();
	return px;
}

/* strimage draws a character-per-pixel image (DrawImage, GIF files):
 * 0, or -1 on error. */
static int strimage(WBinding *w, int x, int y, int width, int height, struct palentry *e, const unsigned char *s,
	size_t len) {
	unsigned pix[256];
	int n = width * height;
	if (!w->window->id || n <= 0)
		return 0;
	for (int c = 0; c < 256; c++)
		if (e[c].used && e[c].valid) {
			char tmp[24]; /* named as rxwin.ri's strimage does */
			snprintf(tmp, sizeof tmp, "%ld,%ld,%ld", e[c].clr.red, e[c].clr.green, e[c].clr.blue);
			pix[c] = sdl_rgb(w, e[c].clr.red, e[c].clr.green, e[c].clr.blue, tmp, NULL);
		}
	unsigned *px = xcalloc((size_t)n, sizeof *px);
	unsigned char *op = xcalloc((size_t)n, 1);
	int ix = 0, iy = 0;
	while (len-- && iy < height) {
		int c = *s++;
		int v = e[c].valid;
		if (v) {
			px[iy * width + ix] = pix[c];
			op[iy * width + ix] = 1;
		}
		if (v || e[c].transpt) {
			if (++ix >= width) {
				ix = 0;
				iy++;
			}
		}
	}
	if (ix > 0) /* pad the final row with bg */
		while (ix < width) {
			px[iy * width + ix] = w->context->bg;
			op[iy * width + ix++] = 1;
		}
	putpixels(w, x, y, width, height, px, op);
	free(px);
	free(op);
	return 0;
}

/* blimage draws a bilevel image ("width,#hex" or "width,~hex"): the hex
 * digits fill each row right to left, as in rxwin.ri. */
static void blimage(WBinding *w, int x, int y, int width, int height, int ch, const unsigned char *s, size_t len) {
	int n = width * height;
	if (!w->window->id || n <= 0)
		return;
	unsigned *px = xcalloc((size_t)n, sizeof *px);
	unsigned char *op = xcalloc((size_t)n, 1);
	unsigned m = (unsigned)width % 4;
	unsigned msk1 = m == 0 ? 8 : 1u << (m - 1);
	unsigned fg = w->context->fg, bg = w->context->bg;
	unsigned ix = (unsigned)width, iy = 0;
	m = msk1;
	while (len-- && iy < (unsigned)height) {
		unsigned c = *s++;
		if (isxdigit((int)c)) {
			if (!isdigit((int)c))
				c += 9;
			while (m > 0) {
				--ix;
				if (c & m) {
					px[iy * width + ix] = fg;
					op[iy * width + ix] = 1;
				} else if (ch != TCH1) {
					px[iy * width + ix] = bg;
					op[iy * width + ix] = 1;
				}
				m >>= 1;
			}
			if (ix == 0) {
				ix = (unsigned)width;
				iy++;
				m = msk1;
			} else
				m = 8;
		}
	}
	if (ix > 0 && ix < (unsigned)width && iy < (unsigned)height)
		while (ix < (unsigned)width) { /* pad the final row */
			px[iy * width + ix] = bg;
			op[iy * width + ix++] = 1;
		}
	putpixels(w, x, y, width, height, px, op);
	free(px);
	free(op);
}

FN(f_DrawImage) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int64_t x = -w->context->dx, y = -w->context->dy;
	/* x or y can be defaulted but s is required; x and y first, so that
	 * the message makes more sense */
	if (n - warg >= 1 && !defint(a[warg], -w->context->dx, &x))
		runerrv(101, a[warg]);
	if (n - warg >= 2 && !defint(a[warg + 1], -w->context->dy, &y))
		runerrv(101, a[warg + 1]);
	if (n - warg < 3)
		runerr(103);
	String d;
	if (!cvstr(a[warg + 2], &d))
		runerrv(103, a[warg + 2]);
	x += w->context->dx;
	y += w->context->dy;
	const unsigned char *s = (const unsigned char *)d.s, *z = s + d.n, *t;
	int width = 0;
	while (s < z && *s == ' ')
		s++;
	while (s < z && isdigit(*s))
		width = 10 * width + *s++ - '0';
	while (s < z && *s == ' ')
		s++;
	if (width == 0 || s >= z || *s++ != ',')
		return false;
	while (s < z && *s == ' ')
		s++;
	if (s >= z)
		return false;
	int c = *s;
	size_t nchars;
	if (c == '#' || c == '~') { /* bilevel */
		s++;
		nchars = 0;
		for (t = s; t < z; t++)
			if (isxdigit(*t))
				nchars++;
			else if (*t != PCH1 && *t != PCH2)
				return false;
		if (nchars == 0)
			return false;
		size_t row = ((size_t)width + 3) / 4;
		if (nchars % row != 0)
			return false;
		blimage(w, (int)x, (int)y, width, (int)(nchars / row), c, s, (size_t)(z - s));
		*out = NULLV;
		return true;
	}
	c = *s++; /* the palette */
	int p = 0;
	while (s < z && isdigit(*s))
		p = 10 * p + *s++ - '0';
	while (s < z && *s == ' ')
		s++;
	if (s >= z || p == 0 || *s++ != ',')
		return false;
	if (c == 'g' && p >= 2 && p <= 256)
		p = -p;
	else if (c != 'c' || p < 1 || p > 6)
		return false;
	struct palentry *e = palsetup(p);
	if (!e)
		runerr(305);
	for (int i = 0; i < 256; i++)
		e[i].used = 0;
	nchars = 0;
	for (t = s; t < z; t++) {
		c = *t;
		e[c].used = 1;
		if (e[c].valid || e[c].transpt)
			nchars++;
		else if (c != PCH1 && c != PCH2)
			return false;
	}
	if (nchars == 0 || nchars % (size_t)width != 0)
		return false;
	if (strimage(w, (int)x, (int)y, width, (int)(nchars / (size_t)width), e, s, (size_t)(z - s)) < 0)
		runerr(305);
	*out = NULLV;
	return true;
}

/* palarg: the palette a value names (palnum): 1 for null, -1 if it is
 * not a string. */
static int palarg(Value v) {
	if (VKIND(v) == KNull)
		return 1;
	String s;
	if (!cvstr(v, &s))
		return -1;
	return palnum(cstr(s));
}

static bool iswindowarg(Value *a, int n) {
	return n > 0 && VKIND(a[0]) == KFile && ((File *)a[0].w.p)->gfx;
}

/* bytesorstr: a string of n bytes. */
static Value bytesorstr(const char *s, size_t n) { return strval(strcopy(s, (int64_t)n)); }

FN(f_PaletteChars) {
	(void)b;
	int warg = iswindowarg(a, n); /* a window is not required */
	int p = n - warg < 1 ? 1 : palarg(a[warg]);
	if (p == -1)
		runerrv(103, a[warg]);
	if (p == 0)
		return false;
	int k;
	const char *s = palchars(p, &k);
	*out = bytesorstr(s, (size_t)k);
	return true;
}

FN(f_PaletteColor) {
	(void)b;
	int warg = iswindowarg(a, n);
	if (n - warg < 2)
		runerr(103);
	int p = palarg(a[warg]);
	if (p == -1)
		runerrv(103, a[warg]);
	if (p == 0)
		return false;
	String d;
	if (!cvstr(a[warg + 1], &d))
		runerrv(103, a[warg + 1]);
	if (d.n != 1)
		runerrv(205, a[warg + 1]);
	struct palentry *e = palsetup(p);
	if (!e)
		runerr(305);
	e += (unsigned char)d.s[0];
	if (!e->valid)
		return false;
	char tmp[48];
	snprintf(tmp, sizeof tmp, "%ld,%ld,%ld", e->clr.red, e->clr.green, e->clr.blue);
	*out = strval(strcopy(tmp, (int64_t)strlen(tmp)));
	return true;
}

FN(f_PaletteKey) {
	(void)b;
	WBinding *w = NULL;
	int warg = 0;
	if (iswindowarg(a, n)) {
		w = ((File *)a[0].w.p)->gfx; /* an explicit window */
		warg = 1;
	} else if (VKIND(kwindow) == KFile && ((File *)kwindow.w.p)->gfx)
		w = ((File *)kwindow.w.p)->gfx; /* &window */
	if (n - warg < 2)
		runerr(103);
	int p = palarg(a[warg]);
	if (p == -1)
		runerrv(103, a[warg]);
	if (p == 0)
		return false;
	int64_t i;
	if (cvint(a[warg + 1], &i))
		return false; /* a mutable colour: gcon has none */
	String s;
	if (!cvstr(a[warg + 1], &s))
		runerrv(103, a[warg + 1]);
	long r, g, bl;
	if (parsecolor(w, cstr(s), &r, &g, &bl) != Succeeded)
		return false;
	*out = bytesorstr(rgbkey(p, r / 65535.0, g / 65535.0, bl / 65535.0), 1);
	return true;
}

/* capture gives a region as 15-bit linear colour, for gif_write,
 * mapping each 8-bit channel through gamma as rxwin.ri does. */
static short *capture(WBinding *w, int x, int y, int width, int height) {
	size_t k = (size_t)width * (size_t)height;
	short *data = xmalloc(k * sizeof *data + 1);
	unsigned char cmap[256];
	double gamma = w->context->gamma;
	for (int i = 0; i < 256; i++)
		cmap[i] = (unsigned char)(31 * pow(i / 255., gamma) + 0.5);
	unsigned *px = getpixels(w, x, y, width, height);
	for (size_t i = 0; i < k; i++) {
		unsigned p = px[i];
		data[i] = (short)((cmap[(p >> 16) & 0xFF] << 10) | (cmap[(p >> 8) & 0xFF] << 5) | cmap[p & 0xFF]);
	}
	free(px);
	return data;
}

/* readall reads a file; false if it cannot. */
static bool readall(const char *name, Buf *b) {
	FILE *f = fopen(name, "rb");
	if (!f)
		return false;
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(b, chunk, k);
	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

FN(f_ReadImage) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (n - warg == 0)
		runerrv(103, NULLV);
	String fname;
	if (!cvstr(a[warg], &fname))
		runerrv(103, a[warg]);
	/* x and y must be integers; they default to the upper left pixel */
	int64_t x = -w->context->dx, y = -w->context->dy;
	if (n - warg >= 2 && !defint(a[warg + 1], -w->context->dx, &x))
		runerrv(101, a[warg + 1]);
	if (n - warg >= 3 && !defint(a[warg + 2], -w->context->dy, &y))
		runerrv(101, a[warg + 2]);
	int p = 0; /* an optional palette */
	if (n - warg >= 4 && VKIND(a[warg + 3]) != KNull) {
		p = palarg(a[warg + 3]);
		if (p == -1)
			runerrv(103, a[warg + 3]);
		if (p == 0)
			return false;
	}
	x += w->context->dx;
	y += w->context->dy;
	unsigned *px;
	unsigned char *op;
	int iw, ih;
	if (!loadimage(w, cstr(fname), p, &px, &op, &iw, &ih))
		return false;
	putpixels(w, (int)x, (int)y, iw, ih, px, op);
	free(px);
	free(op);
	*out = NULLV;
	return true;
}

/* loadimage reads an image file: GIF by Icon's reader (its colours
 * coerced to palette p, if p is not 0), else as the server decodes it
 * (PNG, JPEG, BMP, QOI). The pixels, 0xRRGGBB as w's device colours,
 * opaque where op is nonzero; false if it cannot be read. */
static bool loadimage(WBinding *w, const char *name, int p, unsigned **pxp, unsigned char **opp, int *iw, int *ih) {
	Buf data = {0};
	if (!readall(name, &data))
		return false;
	struct imgdata imd;
	if (gif_read((unsigned char *)data.b, data.n, p, &imd)) {
		free(data.b);
		unsigned pix[256];
		for (int c = 0; c < 256; c++) {
			struct palentry *e = &imd.paltbl[c];
			if (e->used && e->valid) {
				char tmp[24]; /* named as rxwin.ri's strimage does */
				snprintf(tmp, sizeof tmp, "%ld,%ld,%ld", e->clr.red, e->clr.green, e->clr.blue);
				pix[c] = sdl_rgb(w, e->clr.red, e->clr.green, e->clr.blue, tmp, NULL);
			}
		}
		size_t k = (size_t)imd.width * (size_t)imd.height;
		unsigned *px = xcalloc(k + 1, sizeof *px);
		unsigned char *op = xcalloc(k + 1, 1);
		for (size_t i = 0; i < k; i++) {
			int c = imd.data[i];
			if (imd.paltbl[c].valid) {
				px[i] = pix[c];
				op[i] = 1;
			}
		}
		*iw = imd.width;
		*ih = imd.height;
		free(imd.paltbl);
		free(imd.data);
		*pxp = px;
		*opp = op;
		return true;
	}
	/* PNG, JPEG, BMP, QOI: imgcodec.c's, whichever server draws */
	uint32_t *v = data.b ? img_decode((const unsigned char *)data.b, data.n, iw, ih) : NULL;
	free(data.b);
	if (!v)
		return false;
	size_t k = (size_t)*iw * (size_t)*ih;
	unsigned *px = xmalloc(k * sizeof *px + 1);
	unsigned char *op = xmalloc(k + 1);
	for (size_t i = 0; i < k; i++) {
		px[i] = v[i] & 0xFFFFFF;
		op[i] = (v[i] >> 24) >= 128; /* alpha below half: transparent */
	}
	free(v);
	*pxp = px;
	*opp = op;
	return true;
}

/* sendicon gives ws's window its icon (iconimage=), if it has both. */
static void sendicon(WState *ws) {
	if (!ws->iconpx || !ws->id || !ws->win)
		return;
	size_t k = (size_t)ws->iconw * (size_t)ws->iconh;
	gp_begin(P_ICON);
	gp_u32(ws->id);
	gp_i32(ws->iconw);
	gp_i32(ws->iconh);
	for (size_t i = 0; i < k; i++)
		gp_u32(ws->iconpx[i]);
	gp_end();
}

FN(f_WriteImage) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	if (n - warg == 0)
		runerrv(103, NULLV);
	String fname;
	if (!cvstr(a[warg], &fname))
		runerrv(103, a[warg]);
	int x, y, width, height;
	int r = rectargs(w, n, a, warg + 1, &x, &y, &width, &height);
	if (r >= 0)
		runerrv(101, a[r]);
	/* clip to the window, and fail if nothing is left */
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > w->window->width)
		width = w->window->width - x;
	if (y + height > w->window->height)
		height = w->window->height - y;
	if (width <= 0 || height <= 0 || !w->window->id)
		return false;
	const char *name = cstr(fname);
	const char *dot = strrchr(name, '.'), *slash = strrchr(name, '/');
	const char *ext = dot && (!slash || dot > slash) ? dot + 1 : "";
	/* PNG, JPEG, BMP and QOI by their extensions (imgcodec.c), any
	 * other as GIF, Icon's own */
	unsigned char *bytes = NULL;
	size_t blen = 0;
	int st = -1;
	if (img_writes(ext)) {
		unsigned *px = getpixels(w, x, y, width, height);
		st = img_encode(ext, px, width, height, &bytes, &blen);
		free(px);
	}
	if (st < 0) {
		short *data = capture(w, x, y, width, height);
		int ok = gif_write(data, width, height, &bytes, &blen);
		free(data);
		if (!ok)
			return false;
	} else if (st == 0) {
		free(bytes);
		return false;
	}
	FILE *f = fopen(name, "wb");
	bool ok = f && fwrite(bytes, 1, blen, f) == blen;
	if (f && fclose(f) != 0)
		ok = false;
	free(bytes);
	if (!ok)
		return false;
	*out = retwindow(a, warg);
	return true;
}

/* genCurve draws a smooth curve through n points (rwindow.r: from
 * Barry and Goldman, "A Recursive Evaluation Algorithm for a class of
 * Catmull-Rom Splines", 1988), as polylines. */
static void genCurve(WBinding *w, const int *px, const int *py, int n) {
	int *xy = NULL;
	long npoints = 0;
	for (int i = 3; i < n; i++) {
		float ax = px[i] - 3 * px[i - 1] + 3 * px[i - 2] - px[i - 3];
		float ay = py[i] - 3 * py[i - 1] + 3 * py[i - 2] - py[i - 3];
		float bx = 2 * px[i - 3] - 5 * px[i - 2] + 4 * px[i - 1] - px[i];
		float by = 2 * py[i - 3] - 5 * py[i - 2] + 4 * py[i - 1] - py[i];
		int adx = abs(px[i - 1] - px[i - 2]), ady = abs(py[i - 1] - py[i - 2]);
		int steps = (adx > ady ? adx : ady) + 10;
		if (steps + 4 > npoints) {
			free(xy);
			xy = xmalloc((size_t)(steps + 4) * 2 * sizeof *xy);
			npoints = steps + 4;
		}
		float stepsize = 1.0 / steps;
		float stepsize2 = stepsize * stepsize;
		float stepsize3 = stepsize * stepsize2;
		float x = xy[0] = px[i - 2];
		float y = xy[1] = py[i - 2];
		float dx = (stepsize3 * 0.5) * ax + (stepsize2 * 0.5) * bx + (stepsize * 0.5) * (px[i - 1] - px[i - 3]);
		float dy = (stepsize3 * 0.5) * ay + (stepsize2 * 0.5) * by + (stepsize * 0.5) * (py[i - 1] - py[i - 3]);
		float d2x = (stepsize3 * 3) * ax + stepsize2 * bx;
		float d2y = (stepsize3 * 3) * ay + stepsize2 * by;
		float d3x = (stepsize3 * 3) * ax;
		float d3y = (stepsize3 * 3) * ay;
		for (int j = 0; j < steps; j++) {
			x = x + dx;
			y = y + dy;
			dx = dx + d2x;
			dy = dy + d2y;
			d2x = d2x + d3x;
			d2y = d2y + d3y;
			xy[2 * (j + 1)] = (short)(int)x;
			xy[2 * (j + 1) + 1] = (short)(int)y;
		}
		drawxy(w, P_LINES, xy, steps + 1, 2);
	}
	free(xy);
}

FN(f_DrawCurve) {
	(void)b;
	int warg;
	WBinding *w = optwindow(a, n, &warg);
	int c = multiple(n, warg, 2);
	int dx = w->context->dx, dy = w->context->dy;
	int *px = xmalloc(sizeof(int) * (size_t)(c + 2)), *py = xmalloc(sizeof(int) * (size_t)(c + 2));
	if (c > 1) {
		int x0 = cshort(a[warg]), y0 = cshort(a[warg + 1]);
		int xN = cshort(a[n - 2]), yN = cshort(a[n - 1]);
		bool closed = x0 == xN && y0 == yN;
		if (closed) { /* duplicate the next to last point */
			px[0] = cshort(a[n - 4]) + dx;
			py[0] = cshort(a[n - 3]) + dy;
		} else { /* duplicate the first point */
			px[0] = x0 + dx;
			py[0] = y0 + dy;
		}
		int i;
		for (i = 1; i <= c; i++) {
			int base = warg + (i - 1) * 2;
			px[i] = (short)cshort(a[base]) + dx;
			py[i] = (short)cshort(a[base + 1]) + dy;
		}
		if (closed) { /* duplicate the second point */
			px[i] = px[2];
			py[i] = py[2];
		} else { /* duplicate the last point */
			px[i] = px[i - 1];
			py[i] = py[i - 1];
		}
		if (c < 3) {
			int *xy = xmalloc(sizeof(int) * 2 * (size_t)c);
			for (int k = 0; k < c; k++)
				xy[2 * k] = px[k + 1], xy[2 * k + 1] = py[k + 1];
			drawxy(w, P_LINES, xy, c, 2);
			free(xy);
		} else
			genCurve(w, px, py, c + 2);
	}
	free(px);
	free(py);
	*out = retwindow(a, warg);
	return true;
}
/* ---- Registration ---- */

static Builtin fns[] = {
	{.name = "Alert", .fn = f_Alert}, {.name = "Bg", .fn = f_Bg},
	{.name = "DrawCurve", .fn = f_DrawCurve}, {.name = "DrawImage", .fn = f_DrawImage},
	{.name = "PaletteChars", .fn = f_PaletteChars}, {.name = "PaletteColor", .fn = f_PaletteColor},
	{.name = "PaletteKey", .fn = f_PaletteKey}, {.name = "ReadImage", .fn = f_ReadImage},
	{.name = "WriteImage", .fn = f_WriteImage},
	{.name = "Clip", .fn = f_Clip}, {.name = "Clone", .fn = f_Clone},
	{.name = "CopyArea", .fn = f_CopyArea}, {.name = "Couple", .fn = f_Couple},
	{.name = "DrawArc", .fn = f_DrawArc}, {.name = "DrawCircle", .fn = f_DrawCircle},
	{.name = "DrawLine", .fn = f_DrawLine}, {.name = "DrawPoint", .fn = f_DrawPoint},
	{.name = "DrawPolygon", .fn = f_DrawPolygon},
	{.name = "DrawRectangle", .fn = f_DrawRectangle},
	{.name = "DrawSegment", .fn = f_DrawSegment}, {.name = "DrawString", .fn = f_DrawString},
	{.name = "EraseArea", .fn = f_EraseArea}, {.name = "Event", .fn = f_Event},
	{.name = "Fg", .fn = f_Fg}, {.name = "FillArc", .fn = f_FillArc},
	{.name = "FillCircle", .fn = f_FillCircle}, {.name = "FillPolygon", .fn = f_FillPolygon},
	{.name = "FillRectangle", .fn = f_FillRectangle}, {.name = "Font", .fn = f_Font},
	{.name = "GotoRC", .fn = f_GotoRC}, {.name = "GotoXY", .fn = f_GotoXY},
	{.name = "Lower", .fn = f_Lower}, {.name = "Pending", .fn = f_Pending},
	{.name = "Pixel", .gen = f_Pixel}, {.name = "QueryPointer", .gen = f_QueryPointer},
	{.name = "Raise", .fn = f_Raise}, {.name = "TextWidth", .fn = f_TextWidth},
	{.name = "Uncouple", .fn = f_Uncouple}, {.name = "WAttrib", .gen = f_WAttrib},
	{.name = "WFlush", .fn = f_WFlush}, {.name = "WSync", .fn = f_WSync},
	{.name = "Pattern", .fn = f_Pattern}, {.name = "ColorValue", .fn = f_ColorValue},
	{.name = "WDefault", .fn = f_WDefault}, {.name = "Active", .fn = f_Active},
	{.name = "FreeColor", .fn = f_FreeColor}, {.name = "NewColor", .fn = f_NewColor},
	{.name = "Color", .fn = f_Color},
};

static Builtin ops[] = {
	{.name = "&window", .key = "&window", .fn = k_window},
	{.name = "&x", .key = "&x", .fn = k_x}, {.name = "&y", .key = "&y", .fn = k_y},
	{.name = "&row", .key = "&row", .fn = k_row}, {.name = "&col", .key = "&col", .fn = k_col},
	{.name = "&interval", .key = "&interval", .fn = k_interval},
	{.name = "&control", .key = "&control", .fn = k_control},
	{.name = "&shift", .key = "&shift", .fn = k_shift},
	{.name = "&meta", .key = "&meta", .fn = k_meta},
};

void graphics_init(void) {
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
}
