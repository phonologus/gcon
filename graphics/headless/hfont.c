/*
 * hfont.c -- the headless display server's fonts: X's misc-fixed
 * bitmap fonts (deps/fixed, compiled in by ../raster/bdf2c.sh as
 * fixedfonts.inc), Latin-1. MIT licence, see ../LICENSE.md.
 *
 * "fixed" is 6x13, as X's. Icon's portable families (mono, typewriter,
 * sans, serif) take the font nearest the size asked for, with its bold
 * (B) and oblique (O) faces where it has them; a font's own name (6x13,
 * 7x13B) opens it, as an X font name would; any other name fails, as it
 * does on an X server without that font. The default size is 14.
 */
#include <stdlib.h>
#include <string.h>
#include "sdlgfx.h"
#include "fixedfonts.inc"

const BdfFont *h_font(void *f) { return f; }

void *ft_open(const char *family, int bold, int italic, int size)
{
   const BdfFont *best = NULL;
   if (!strcmp(family, "fixed"))
      family = "6x13";
   for (int i = 0; i < nbdffonts; i++)
      if (!strcmp(family, bdffonts[i].name))
         return (void *)&bdffonts[i];
   if (strcmp(family, "mono") && strcmp(family, "typewriter") && strcmp(family, "sans")
       && strcmp(family, "serif"))
      return NULL;
   if (size <= 0)
      size = 14;
   /* the nearest size with the faces asked for; failing that, without
    * the oblique, then without the bold; the first of equals */
   for (int pass = 0; pass < 3 && !best; pass++) {
      int wantb = pass < 2 ? bold : 0, wanto = pass < 1 ? italic : 0;
      for (int i = 0; i < nbdffonts; i++) {
         const BdfFont *f = &bdffonts[i];
         if (f->bold != wantb || f->italic != wanto)
            continue;
         if (!best || abs(f->pixel - size) < abs(best->pixel - size))
            best = f;
      }
   }
   return (void *)best;
}

void ft_metrics(void *f, int *ascent, int *descent, int *maxwidth)
{
   const BdfFont *b = f;
   *ascent = b ? b->ascent : 0;
   *descent = b ? b->descent : 0;
   *maxwidth = b ? b->maxw : 0;
}

int ft_width(void *f, const char *s, int n)
{
   const BdfFont *b = f;
   int w = 0;
   if (!b)
      return 0;
   for (int i = 0; i < n; i++)
      w += b->g[(unsigned char)s[i]].dw;
   return w;
}
