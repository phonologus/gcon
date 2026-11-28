/*
 * rfont.c -- fonts for the canvases of raster/: X's misc-fixed bitmap
 * fonts (deps/fixed, compiled in by bdf2c.sh as fixedfonts.inc) and,
 * where the server can find the platform's fonts (RFONT_LOOKUP: its
 * font_lookup, CoreText or fontconfig, ../src/fontmac.c, fontlinux.c),
 * TrueType and CFF outline fonts, rasterized by stb_truetype (deps/stb,
 * public domain). Text is Latin-1, one byte a character; no hinting, no
 * OpenType layout, but kerning (the kern table, and GPOS pairs). MIT
 * licence, see ../LICENSE.md.
 *
 * Without a lookup (the headless server): "fixed" is X's 6x13; Icon's
 * portable families (mono, typewriter, sans, serif) take the misc-fixed
 * font nearest the size, with its bold (B) and oblique (O) faces; any
 * other name fails. With one (the SDL and Wayland servers): every name
 * goes to the lookup, as the SDL server's fonts did ("fixed" and "mono"
 * the platform's monospace), and only a misc-fixed font's own name
 * (6x13, 7x13B) opens it, in either case.
 *
 * An outline font's size is its em in pixels; its ascent and descent
 * are the hhea table's, rounded up; its maximum width is over the
 * printable Latin-1 characters, as X's max_bounds.width over a core
 * font's repertoire. A string's width is its advances and kerning,
 * rounded once. Glyphs are rendered at the canvas's scale and cached.
 */
#include "rfont.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdlgfx.h"
#include "bdf.h"
#include "fixedfonts.inc"

#ifdef RFONT_LOOKUP
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

int font_lookup(const char *family, int bold, int italic, char *path, int pathlen, char *psname, int pslen);
#endif

/* a glyph rendered at one scale */
typedef struct {
   int done;
   unsigned char *bm;		/* w * h coverage, or NULL if empty */
   int w, h, x0, y0;		/* its box, device pixels from the pen at the baseline */
} Glyph;

typedef struct GlyphSet {
   int s;			/* the device scale */
   Glyph g[256];
   struct GlyphSet *next;
} GlyphSet;

typedef struct {
   const BdfFont *bdf;		/* a bitmap font; else an outline one */
   int ascent, descent, maxw;
#ifdef RFONT_LOOKUP
   stbtt_fontinfo info;
   float scale;			/* font units to logical pixels */
   int gid[256];		/* glyph indices */
   float adv[256];		/* advances, logical pixels */
   GlyphSet *sets;
#endif
} Font;

/* ---------- bitmap fonts ---------- */

static Font *bitmapfont(const BdfFont *b)
{
   static Font *made[64];
   for (int i = 0; i < nbdffonts && i < 64; i++)
      if (&bdffonts[i] == b) {
         if (!made[i] && (made[i] = calloc(1, sizeof *made[i])) != NULL) {
            made[i]->bdf = b;
            made[i]->ascent = b->ascent;
            made[i]->descent = b->descent;
            made[i]->maxw = b->maxw;
         }
         return made[i];
      }
   return NULL;
}

static const BdfFont *bdfnamed(const char *name)
{
   for (int i = 0; i < nbdffonts; i++)
      if (!strcmp(name, bdffonts[i].name))
         return &bdffonts[i];
   return NULL;
}

#ifndef RFONT_LOOKUP
/* the misc-fixed font nearest size with the faces asked for; failing
 * that, without the oblique, then without the bold; the first of equals */
static const BdfFont *bdfnearest(int bold, int italic, int size)
{
   const BdfFont *best = NULL;
   if (size <= 0)
      size = 14;
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
   return best;
}
#endif

/* ---------- outline fonts ---------- */

#ifdef RFONT_LOOKUP
/* the font files read, kept: a font is opened at many sizes */
typedef struct File {
   char *path;
   unsigned char *data;
   struct File *next;
} File;

static File *files;

static unsigned char *readfile(const char *path)
{
   for (File *f = files; f; f = f->next)
      if (!strcmp(f->path, path))
         return f->data;
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   unsigned char *b = NULL;
   size_t n = 0, cap = 0, k;
   for (;;) {
      if (n == cap) {
         unsigned char *nb = realloc(b, cap = cap ? 2 * cap : 1 << 20);
         if (!nb) {
            free(b);
            fclose(fp);
            return NULL;
         }
         b = nb;
      }
      if ((k = fread(b + n, 1, cap - n, fp)) == 0)
         break;
      n += k;
   }
   fclose(fp);
   File *f = malloc(sizeof *f);
   if (!f) {
      free(b);
      return NULL;
   }
   size_t pn = strlen(path) + 1;
   if (!(f->path = malloc(pn))) { /* strdup is not strict C */
      free(f);
      free(b);
      return NULL;
   }
   memcpy(f->path, path, pn);
   f->data = b;
   f->next = files;
   files = f;
   return b;
}

/* whether the face's PostScript name (name ID 6) is ps */
static int psnamed(const stbtt_fontinfo *info, const char *ps)
{
   int len;
   const char *s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MICROSOFT,
      STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 6);
   if (s) {			/* UTF-16BE */
      size_t n = strlen(ps);
      if ((size_t)len != 2 * n)
         return 0;
      for (size_t i = 0; i < n; i++)
         if (s[2 * i] != 0 || s[2 * i + 1] != ps[i])
            return 0;
      return 1;
   }
   s = stbtt_GetFontNameString(info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
      STBTT_MAC_LANG_ENGLISH, 6);
   return s && (size_t)len == strlen(ps) && !memcmp(s, ps, (size_t)len);
}

static Font *outlinefont(const char *path, const char *ps, int size)
{
   unsigned char *data = readfile(path);
   Font *f;
   int n, found = -1;
   if (!data)
      return NULL;
   if (!(f = calloc(1, sizeof *f)))
      return NULL;
   n = stbtt_GetNumberOfFonts(data);
   if (n < 1)
      n = 1;
   for (int i = 0; i < n && found < 0; i++) {
      int off = stbtt_GetFontOffsetForIndex(data, i);
      if (off >= 0 && stbtt_InitFont(&f->info, data, off) && psnamed(&f->info, ps))
         found = i;
   }
   if (found < 0) {			/* the first face, then */
      int off = stbtt_GetFontOffsetForIndex(data, 0);
      if (off < 0 || !stbtt_InitFont(&f->info, data, off)) {
         free(f);
         return NULL;
      }
   }
   int asc, desc, gap;
   f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)(size > 0 ? size : 14));
   stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
   f->ascent = (int)ceil(asc * f->scale);
   f->descent = (int)ceil(-desc * f->scale);
   for (int c = 0; c < 256; c++) {
      int adv, lsb;
      f->gid[c] = stbtt_FindGlyphIndex(&f->info, c);
      stbtt_GetGlyphHMetrics(&f->info, f->gid[c], &adv, &lsb);
      f->adv[c] = adv * f->scale;
      if (c >= 32 && (c < 127 || c >= 160) && (int)lroundf(f->adv[c]) > f->maxw)
         f->maxw = (int)lroundf(f->adv[c]);
   }
   if (f->maxw == 0)
      f->maxw = size;
   return f;
}
#endif

/* ---------- the interface ---------- */

void *ft_open(const char *family, int bold, int italic, int size)
{
   const BdfFont *b = bdfnamed(family);
   if (b)
      return bitmapfont(b);
#ifdef RFONT_LOOKUP
   char path[4096], ps[256];
   if (!font_lookup(family, bold, italic, path, sizeof path, ps, sizeof ps))
      return NULL;
   return outlinefont(path, ps, size);
#else
   if (!strcmp(family, "fixed"))
      return bitmapfont(bdfnamed("6x13"));
   if (strcmp(family, "mono") && strcmp(family, "typewriter") && strcmp(family, "sans")
       && strcmp(family, "serif"))
      return NULL;
   return bitmapfont(bdfnearest(bold, italic, size));
#endif
}

void ft_metrics(void *fp, int *ascent, int *descent, int *maxwidth)
{
   Font *f = fp;
   *ascent = f ? f->ascent : 0;
   *descent = f ? f->descent : 0;
   *maxwidth = f ? f->maxw : 0;
}

/* the advance of the text, logical, unrounded */
static double advance(Font *f, const char *s, int n)
{
   double w = 0;
   for (int i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      if (f->bdf)
         w += f->bdf->g[c].dw;
#ifdef RFONT_LOOKUP
      else {
         w += f->adv[c];
         if (i + 1 < n)
            w += stbtt_GetGlyphKernAdvance(&f->info, f->gid[c], f->gid[(unsigned char)s[i + 1]]) * f->scale;
      }
#endif
   }
   return w;
}

int ft_width(void *fp, const char *s, int n)
{
   Font *f = fp;
   return f && n > 0 ? (int)lround(advance(f, s, n)) : 0;
}

void rf_ink(void *fp, const char *s, int n, int *left, int *right)
{
   Font *f = fp;
   int w = ft_width(fp, s, n);
   *left = 0;
   *right = w;
   if (!f || f->bdf)
      return;
   *left = -f->maxw;			/* room for overhangs, either side */
   *right = w + f->maxw;
}

#ifdef RFONT_LOOKUP
/* the glyph for c at scale s, rendered once */
static Glyph *glyph(Font *f, unsigned char c, int s)
{
   GlyphSet *gs;
   for (gs = f->sets; gs && gs->s != s; gs = gs->next)
      ;
   if (!gs) {
      if (!(gs = calloc(1, sizeof *gs)))
         return NULL;
      gs->s = s;
      gs->next = f->sets;
      f->sets = gs;
   }
   Glyph *g = &gs->g[c];
   if (!g->done) {
      int x0, y0, x1, y1;
      float sc = f->scale * s;
      g->done = 1;
      stbtt_GetGlyphBitmapBox(&f->info, f->gid[c], sc, sc, &x0, &y0, &x1, &y1);
      if (x1 > x0 && y1 > y0 && (g->bm = malloc((size_t)(x1 - x0) * (y1 - y0))) != NULL) {
         g->w = x1 - x0;
         g->h = y1 - y0;
         g->x0 = x0;
         g->y0 = y0;
         stbtt_MakeGlyphBitmap(&f->info, g->bm, g->w, g->h, g->w, sc, sc, f->gid[c]);
      }
   }
   return g;
}
#endif

static void put(uint8_t *mask, int stride, const int r[4], int x, int y, uint8_t v)
{
   if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) {
      uint8_t *m = mask + (size_t)y * stride + x;
      if (v > *m)
         *m = v;
   }
}

void rf_draw(void *fp, uint8_t *mask, int stride, const int r[4], int s, int x, int y, const char *str, int n)
{
   Font *f = fp;
   if (!f)
      return;
   if (f->bdf) {
      for (int i = 0, pen = x; i < n; i++) {
         const BdfGlyph *gl = &f->bdf->g[(unsigned char)str[i]];
         if (gl->bits >= 0) {
            int bpr = (gl->w + 7) / 8, top = y - gl->yo - gl->h;
            for (int row = 0; row < gl->h; row++) {
               const unsigned char *b = f->bdf->bits + gl->bits + row * bpr;
               for (int col = 0; col < gl->w; col++)
                  if (b[col >> 3] & (0x80 >> (col & 7)))
                     for (int yy = 0; yy < s; yy++)
                        for (int xx = 0; xx < s; xx++)
                           put(mask, stride, r, (pen + gl->xo + col) * s + xx, (top + row) * s + yy, 255);
            }
         }
         pen += gl->dw;
      }
      return;
   }
#ifdef RFONT_LOOKUP
   double pen = x;
   for (int i = 0; i < n; i++) {
      unsigned char c = (unsigned char)str[i];
      Glyph *g = glyph(f, c, s);
      if (g && g->bm) {
         int ox = (int)lround(pen * s) + g->x0, oy = y * s + g->y0;
         for (int row = 0; row < g->h; row++)
            for (int col = 0; col < g->w; col++)
               if (g->bm[row * g->w + col])
                  put(mask, stride, r, ox + col, oy + row, g->bm[row * g->w + col]);
      }
      pen += f->adv[c];
      if (i + 1 < n)
         pen += stbtt_GetGlyphKernAdvance(&f->info, f->gid[c], f->gid[(unsigned char)str[i + 1]]) * f->scale;
   }
#endif
}
