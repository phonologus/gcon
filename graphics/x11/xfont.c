/*
 * xfont.c -- the X11 display server's fonts (sdlgfx.h's ft_*): X core
 * fonts, chosen as Icon's X11 backend chooses them.
 * MIT licence, see ../LICENSE.md; alc_font, findfont, okfont, fieldcmp
 * and fontcmp follow Icon 9.5's icon/src/runtime/rxrsc.ri (public
 * domain), given the family, weight, slant and size the protocol's FONT
 * carries (the client parses Icon's font names).
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x11.h"

enum { DEFAULTFONTSIZE = 14 };

/* Icon's font flags (icon/src/h/graphics.h), those used here */
enum {
   FONTFLAG_MONO = 0x1, FONTFLAG_PROPORTIONAL = 0x2, FONTFLAG_SANS = 0x4,
   FONTFLAG_SERIF = 0x8, FONTFLAG_ITALIC = 0x20, FONTFLAG_BOLD = 0x800,
};

/* the fields of an X Logical XFnt Description (icon/src/h/xwin.h) */
enum {
   XLFD_Foundry = 1, XLFD_Family = 2, XLFD_Weight = 3, XLFD_Slant = 4,
   XLFD_SetWidth = 5, XLFD_AddStyle = 6, XLFD_Size = 7, XLFD_PointSize = 8,
   XLFD_Spacing = 11, XLFD_CharSet = 13,
};

typedef struct {
   XFontStruct *fs;
   char *name;			/* as loaded: Arizona's WAttrib("font") */
} XFnt;

XFontStruct *x_fontstruct(void *f) { return f ? ((XFnt *)f)->fs : NULL; }

/* field i of XLFD s */
static const char *xlfd_field(const char *s, int i)
{
   int j = 0;
   while (j < i) {
      if (*s == '\0')
         return "";
      if (*s++ == '-')
         j++;
   }
   return s;
}

/* the font's size, a scalable font's being n */
static int xlfd_size(const char *s, int n)
{
   const char *f = xlfd_field(s, XLFD_Size);
   int z;
   if (!*f)
      return 0;
   z = atoi(f);
   return z != 0 ? z : n;
}

static XFnt *tryfont(const char *s)
{
   XFontStruct *fs = XLoadQueryFont(xdpy, s);
   XFnt *f;
   if (!fs)
      return NULL;
   if (!(f = malloc(sizeof *f))) {
      XFreeFont(xdpy, fs);
      return NULL;
   }
   f->fs = fs;
   if (!(f->name = strdup(s))) {
      XFreeFont(xdpy, fs);
      free(f);
      return NULL;
   }
   return f;
}

/* what XLFD patterns cannot filter: an explicit size, monospacing */
static int okfont(const char *spec, int size, int flags)
{
   if (size > 0 && xlfd_size(spec, size) != size)
      return 0;
   if ((flags & FONTFLAG_MONO) && xlfd_field(spec, XLFD_Spacing)[0] == 'p')
      return 0;
   return 1;
}

/* <0 if font1 matches value in field n and font2 does not, >0 if the
 * reverse, else 0 */
static int fieldcmp(const char *font1, const char *font2, const char *value, int field)
{
   size_t len = strlen(value);
   int r1 = strncmp(xlfd_field(font1, field), value, len) == 0;
   int r2 = strncmp(xlfd_field(font2, field), value, len) == 0;
   return r2 - r1;
}

/* <0 if font1 is better, >0 if font2 is, else 0 */
static int fontcmp(const char *font1, const char *font2, int size, int flags)
{
   int n;
#define PREFER(s, f) do { int r = fieldcmp(font1, font2, s, f); if (r != 0) return r; } while (0)
#define SPURN(s, f) do { int r = fieldcmp(font1, font2, s, f); if (r != 0) return -r; } while (0)
   n = abs(size - xlfd_size(font1, size)) - abs(size - xlfd_size(font2, size));
   if (n != 0)
      return n;
   if (flags & FONTFLAG_SANS) {
      PREFER("sans", XLFD_AddStyle);
      SPURN("serif", XLFD_AddStyle);
   }
   else if (flags & FONTFLAG_SERIF) {
      PREFER("serif", XLFD_AddStyle);
      SPURN("sans", XLFD_AddStyle);
   }
   PREFER("r", XLFD_Slant);
   PREFER("medium", XLFD_Weight);
   SPURN("demi", XLFD_Weight);
   PREFER("normal", XLFD_SetWidth);
   PREFER("iso8859", XLFD_CharSet);
   SPURN("0", XLFD_PointSize);
   PREFER("adobe", XLFD_Foundry);
   return 0;
#undef PREFER
#undef SPURN
}

/* the best font of the family matching the flags and size */
static XFnt *findfont(const char *family, int size, int flags)
{
   char spec[1024];
   const char *weight = flags & FONTFLAG_BOLD ? "bold" : "*";
   const char *slant = flags & FONTFLAG_ITALIC ? "i" : "*";
   const char *spacing = flags & FONTFLAG_PROPORTIONAL ? "p" : "*";
   int n, champ, bestsize = size > 0 ? size : DEFAULTFONTSIZE;
   char **list;
   const char *p;

   snprintf(spec, sizeof spec, "-*-%s-%s-%s-*-*-*-*-*-*-%s-*-*-*", family, weight, slant, spacing);
   list = XListFonts(xdpy, spec, 2500, &n);
   if (!list)
      return NULL;
   champ = 0;
   while (champ < n && !okfont(list[champ], size, flags))
      champ++;
   if (champ >= n) {
      XFreeFontNames(list);
      return NULL;
   }
   for (int c = champ + 1; c < n; c++)
      if (okfont(list[c], size, flags) && fontcmp(list[c], list[champ], bestsize, flags) < 0)
         champ = c;
   /* a scalable font: at the size wanted */
   p = xlfd_field(list[champ], XLFD_Size);
   if (p[0] == '0' && p[1] == '-')
      snprintf(spec, sizeof spec, "%.*s%d%s", (int)(p - list[champ]), list[champ], bestsize, p + 1);
   else
      snprintf(spec, sizeof spec, "%s", list[champ]);
   XFreeFontNames(list);
   return tryfont(spec);
}

void *ft_open(const char *family, int bold, int italic, int size)
{
   const char *std = NULL;
   int flags = (bold ? FONTFLAG_BOLD : 0) | (italic ? FONTFLAG_ITALIC : 0);
   XFnt *f;

   if (!x_open())
      return NULL;
   if (!strcmp(family, "fixed"))
      return tryfont("fixed");
   if (!strcmp(family, "mono")) {
      std = "lucidatypewriter";
      flags |= FONTFLAG_MONO | FONTFLAG_SANS;
   }
   else if (!strcmp(family, "typewriter")) {
      std = "courier";
      flags |= FONTFLAG_MONO | FONTFLAG_SERIF;
   }
   else if (!strcmp(family, "sans")) {
      std = "helvetica";
      flags |= FONTFLAG_PROPORTIONAL | FONTFLAG_SANS;
   }
   else if (!strcmp(family, "serif")) {
      std = "times";
      flags |= FONTFLAG_PROPORTIONAL | FONTFLAG_SERIF;
   }
   if (std) {			/* the preferred family, then any */
      if ((f = findfont(std, size, flags)) || (f = findfont("*", size, flags)))
         return f;
   }
   else if ((f = findfont(family, size, flags)))
      return f;
   return tryfont(family);	/* an X font name */
}

void ft_metrics(void *f, int *ascent, int *descent, int *maxwidth)
{
   XFontStruct *fs = x_fontstruct(f);
   *ascent = fs ? fs->ascent : 0;
   *descent = fs ? fs->descent : 0;
   *maxwidth = fs ? fs->max_bounds.width : 0;
}

int ft_width(void *f, const char *s, int n)
{
   XFontStruct *fs = x_fontstruct(f);
   return fs && n > 0 ? XTextWidth(fs, s, n) : 0;
}

const char *ft_name(void *f) { return f ? ((XFnt *)f)->name : ""; }
