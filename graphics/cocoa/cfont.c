/*
 * cfont.c -- the Cocoa display server's fonts (sdlgfx.h's ft_open and
 * ft_metrics; ft_width is ccanvas.c's, which shapes the same way):
 * CoreText. MIT licence, see ../LICENSE.md.
 *
 * Icon's portable families map to the Mac's fonts as in the SDL server
 * (../src/fontmac.c): mono and fixed Menlo, typewriter Courier, sans
 * Helvetica, serif Times. Any other name must be the found font's family
 * or full name (ignoring case): CoreText would otherwise substitute a
 * fallback silently, where X fails the request. The metrics are the SDL
 * server's (../raster/rfont.c): ascent and descent rounded up, and the
 * maximum width over the printable Latin-1 characters, as X's
 * max_bounds.width over a core font's repertoire.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cocoa.h"

typedef struct {
   CTFontRef f;
   int ascent, descent, maxw;
   char name[256];		/* "Full Name,size" (ft_name) */
} Fnt;

/* s as UTF-8 into buf ("" if none) */
static void cfstr(CFStringRef s, char *buf, size_t n)
{
   buf[0] = '\0';
   if (s) {
      if (!CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8))
         buf[0] = '\0';
      CFRelease(s);
   }
}

CTFontRef c_font(void *f) { return f ? ((Fnt *)f)->f : NULL; }

static const char *generic(const char *f)
{
   if (!strcmp(f, "mono") || !strcmp(f, "fixed")) return "Menlo";
   if (!strcmp(f, "typewriter")) return "Courier";
   if (!strcmp(f, "sans")) return "Helvetica";
   if (!strcmp(f, "serif")) return "Times";
   return NULL;
}

static int same(CFStringRef a, CFStringRef b)
{
   return a && b && CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

void *ft_open(const char *family, int bold, int italic, int size)
{
   const char *g = generic(family);
   CFStringRef name = CFStringCreateWithCString(NULL, g ? g : family, kCFStringEncodingUTF8);
   CTFontSymbolicTraits traits = 0;
   CTFontRef f, t;
   Fnt *r;

   if (!name)
      return NULL;
   f = CTFontCreateWithName(name, size > 0 ? size : 14, NULL);
   CFRelease(name);
   if (!f)
      return NULL;
   if (!g) {				/* must be the font asked for */
      CFStringRef want = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
      CFStringRef fam = CTFontCopyFamilyName(f), full = CTFontCopyFullName(f);
      int ok = same(want, fam) || same(want, full);
      if (want) CFRelease(want);
      if (fam) CFRelease(fam);
      if (full) CFRelease(full);
      if (!ok) {
         CFRelease(f);
         return NULL;
      }
   }
   if (bold) traits |= kCTFontBoldTrait;
   if (italic) traits |= kCTFontItalicTrait;
   if (traits && (t = CTFontCreateCopyWithSymbolicTraits(f, 0.0, NULL, traits, traits))) {
      CFRelease(f);
      f = t;
   }
   if (!(r = calloc(1, sizeof *r))) {
      CFRelease(f);
      return NULL;
   }
   r->f = f;
   r->ascent = (int)ceil(CTFontGetAscent(f));
   r->descent = (int)ceil(CTFontGetDescent(f));
   for (int ch = 32; ch < 256; ch++) {
      char s1 = (char)ch;
      int w;
      if (ch >= 127 && ch < 160)
         continue;
      if ((w = ft_width(r, &s1, 1)) > r->maxw)
         r->maxw = w;
   }
   if (r->maxw == 0)
      r->maxw = size;
   {
      char full[200];			/* the full name, which ft_open finds again */
      cfstr(CTFontCopyFullName(f), full, sizeof full);
      snprintf(r->name, sizeof r->name, "%s,%d", full, size > 0 ? size : 14);
   }
   return r;
}

const char *ft_name(void *f) { return f ? ((Fnt *)f)->name : ""; }

void ft_metrics(void *f, int *ascent, int *descent, int *maxwidth)
{
   Fnt *r = f;
   *ascent = r ? r->ascent : 0;
   *descent = r ? r->descent : 0;
   *maxwidth = r ? r->maxw : 0;
}
