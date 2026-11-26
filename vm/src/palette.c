/*
 * Palettes (gimg.h): Icon's rcolor.r (public domain), its palette part,
 * as it is but for palnum, which takes a C string.
 */
#include "gimg.h"

#include <stdio.h>
#include <stdlib.h>

const unsigned char allchars[256] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
	64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
	80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
	96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
	112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
	144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
	160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
	192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
	208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
	224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
	240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};

/*
 * c<n>list - the characters of the palettes that are not contiguous ASCII
 */
static char c1list[] = "0123456789?!nNAa#@oOBb$%pPCc&|\
qQDd,.rREe;:sSFf+-tTGg*/uUHh`'vVIi<>wWJj()xXKk[]yYLl{}zZMm^=";
static char c2list[] = "kbgcrmywx";
static char c3list[] = "@ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
static char c4list[] =
   "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz{}$%&*+-/?@";

/*
 * cgrays -- lists of grayscales contained within color palettes
 */
static char *cgrays[] = { "0123456", "kxw", "@abMcdZ", "0$%&L*+-g/?@}",
"\0}~\177\200\37\201\202\203\204>\205\206\207\210]\211\212\213\214|",
"\0\330\331\332\333\334+\335\336\337\340\341V\342\343\344\345\346\201\
\347\350\351\352\353\254\354\355\356\357\360\327" };

/*
 * c1cube - a precomputed mapping from a color cube to chars in c1 palette
 *
 * This is 10x10x10 cube (A Thousand Points of Light).
 */
#define C1Side 10			/* length of one side of C1 cube */
static char c1cube[] = {
   '0', '0', 'w', 'w', 'w', 'W', 'W', 'W', 'J', 'J', '0', '0', 'v', 'v', 'v',
   'W', 'W', 'W', 'J', 'J', 's', 't', 't', 'v', 'v', 'V', 'V', 'V', 'V', 'J',
   's', 't', 't', 'u', 'u', 'V', 'V', 'V', 'V', 'I', 's', 't', 't', 'u', 'u',
   'V', 'V', 'V', 'I', 'I', 'S', 'S', 'T', 'T', 'T', 'U', 'U', 'U', 'I', 'I',
   'S', 'S', 'T', 'T', 'T', 'U', 'U', 'U', 'U', 'I', 'S', 'S', 'T', 'T', 'T',
   'U', 'U', 'U', 'U', 'H', 'F', 'F', 'T', 'T', 'G', 'G', 'U', 'U', 'H', 'H',
   'F', 'F', 'F', 'G', 'G', 'G', 'G', 'H', 'H', 'H', '0', '0', 'x', 'x', 'x',
   'W', 'W', 'W', 'J', 'J', '!', '1', '1', 'v', 'v', 'W', 'W', 'W', 'J', 'J',
   'r', '1', '1', 'v', 'v', 'V', 'V', 'V', 'j', 'j', 'r', 'r', 't', 'u', 'u',
   'V', 'V', 'V', 'j', 'j', 'r', 'r', 't', 'u', 'u', 'V', 'V', 'V', 'I', 'I',
   'S', 'S', 'T', 'T', 'T', 'U', 'U', 'U', 'I', 'I', 'S', 'S', 'T', 'T', 'T',
   'U', 'U', 'U', 'i', 'i', 'S', 'S', 'T', 'T', 'T', 'U', 'U', 'U', 'i', 'i',
   'F', 'F', 'f', 'f', 'G', 'G', 'g', 'g', 'H', 'H', 'F', 'F', 'f', 'f', 'G',
   'G', 'g', 'g', 'H', 'H', 'n', 'z', 'x', 'x', 'x', 'X', 'X', 'X', 'X', 'J',
   '!', '1', '1', 'x', 'x', 'X', 'X', 'X', 'j', 'j', 'p', '1', '1', '2', '2',
   ')', 'V', 'j', 'j', 'j', 'r', 'r', '2', '2', '2', ')', 'V', 'j', 'j', 'j',
   'r', 'r', '2', '2', '2', '>', '>', '>', 'j', 'j', 'R', 'R', '-', '-', '/',
   '/', '>', '>', 'i', 'i', 'R', 'R', 'R', 'T', '/', '/', '\'','i', 'i', 'i',
   'R', 'R', 'f', 'f', '/', '/', 'g', 'g', 'i', 'i', 'R', 'f', 'f', 'f', 'f',
   'g', 'g', 'g', 'h', 'h', 'F', 'f', 'f', 'f', 'f', 'g', 'g', 'g', 'h', 'h',
   'n', 'z', 'z', 'y', 'y', 'X', 'X', 'X', 'X', 'K', 'o', 'o', 'z', 'y', 'y',
   'X', 'X', 'X', 'j', 'j', 'p', 'p', '2', '2', '2', ')', 'X', 'j', 'j', 'j',
   'q', 'q', '2', '2', '2', ')', ')', 'j', 'j', 'j', 'q', 'q', '2', '2', '2',
   '>', '>', '>', 'j', 'j', 'R', 'R', '-', '-', '/', '/', '>', '>', 'i', 'i',
   'R', 'R', 'R', '-', '/', '/', '\'','\'','i', 'i', 'R', 'R', 'f', 'f', '/',
   '/', '\'','g', 'i', 'i', 'R', 'f', 'f', 'f', 'f', 'g', 'g', 'g', 'h', 'h',
   'E', 'f', 'f', 'f', 'f', 'g', 'g', 'g', 'h', 'h', 'n', 'z', 'z', 'y', 'y',
   'X', 'X', 'X', 'K', 'K', 'o', 'o', 'z', 'y', 'y', 'X', 'X', 'X', 'K', 'K',
   '?', '?', '?', '2', '2', ']', ']', ']', 'j', 'j', 'q', 'q', '2', '2', '2',
   ']', ']', ']', 'j', 'j', 'q', 'q', '2', '2', '3', '3', '>', '>', 'j', 'j',
   'R', 'R', ':', ':', '3', '3', '>', '>', 'i', 'i', 'R', 'R', ':', ':', ':',
   '/', '\'','\'','i', 'i', 'R', 'R', ':', ':', ':', '/', '\'','\'','i', 'i',
   'E', 'E', 'f', 'f', 'f', 'g', 'g', 'g', 'h', 'h', 'E', 'E', 'f', 'f', 'f',
   'g', 'g', 'g', 'h', 'h', 'N', 'N', 'Z', 'Z', 'Z', 'Y', 'Y', 'Y', 'K', 'K',
   'O', 'O', 'Z', 'Z', 'Z', 'Y', 'Y', 'Y', 'K', 'K', '?', '?', '?', '@', '=',
   ']', ']', ']', 'k', 'k', 'P', 'P', '@', '@', '=', ']', ']', ']', 'k', 'k',
   'P', 'P', '%', '%', '%', '3', ']', ']', 'k', 'k', 'Q', 'Q', '|', '|', '3',
   '3', '4', '4', '(', '(', 'Q', 'Q', ':', ':', ':', '4', '4', '4', '(', '(',
   'Q', 'Q', ':', ':', ':', '4', '4', '4', '<', '<', 'E', 'E', 'e', 'e', 'e',
   '+', '+', '*', '*', '<', 'E', 'E', 'e', 'e', 'e', '+', '+', '*', '*', '`',
   'N', 'N', 'Z', 'Z', 'Z', 'Y', 'Y', 'Y', 'Y', 'K', 'O', 'O', 'Z', 'Z', 'Z',
   'Y', 'Y', 'Y', 'k', 'k', 'O', 'O', 'O', 'Z', '=', '=', '}', 'k', 'k', 'k',
   'P', 'P', 'P', '@', '=', '=', '}', '}', 'k', 'k', 'P', 'P', '%', '%', '%',
   '=', '}', '}', 'k', 'k', 'Q', 'Q', '|', '|', '|', '4', '4', '4', '(', '(',
   'Q', 'Q', '.', '.', '.', '4', '4', '4', '(', '(', 'Q', 'Q', 'e', '.', '.',
   '4', '4', '4', '<', '<', 'Q', 'e', 'e', 'e', 'e', '+', '+', '*', '*', '<',
   'E', 'e', 'e', 'e', 'e', '+', '+', '*', '*', '`', 'N', 'N', 'Z', 'Z', 'Z',
   'Y', 'Y', 'Y', 'Y', 'L', 'O', 'O', 'Z', 'Z', 'Z', 'Y', 'Y', 'Y', 'k', 'k',
   'O', 'O', 'O', 'a', '=', '=', 'm', 'k', 'k', 'k', 'P', 'P', 'a', 'a', '=',
   '=', '}', 'k', 'k', 'k', 'P', 'P', '%', '%', '%', '=', '}', '8', '8', '8',
   'Q', 'Q', '|', '|', '|', '4', '4', '8', '8', '8', 'Q', 'Q', 'c', '.', '.',
   '4', '4', '4', '[', '[', 'Q', 'Q', 'c', 'c', '9', '9', '4', '5', '5', '<',
   'Q', 'e', 'e', 'e', 'e', ';', ';', '5', '5', '<', 'D', 'e', 'e', 'e', 'e',
   ';', ';', ';', '*', '`', 'A', 'A', 'Z', 'Z', 'M', 'M', 'Y', 'Y', 'L', 'L',
   'A', 'A', 'a', 'a', 'M', 'M', 'm', 'm', 'L', 'L', 'B', 'B', 'a', 'a', 'a',
   'm', 'm', 'm', 'l', 'l', 'B', 'B', 'a', 'a', 'a', 'm', 'm', 'm', 'l', 'l',
   'C', 'C', 'b', 'b', 'b', '7', '7', '7', '8', '8', 'C', 'C', 'b', 'b', 'b',
   '7', '7', '^', '[', '[', 'Q', 'c', 'c', 'c', 'c', '#', '#', '^', '[', '[',
   'Q', 'c', 'c', 'c', '9', '9', '$', '5', '5', '[', 'D', 'D', 'd', 'd', '9',
   '&', '&', '5', '5', '6', 'D', 'D', 'd', 'd', 'd', ';', ';', ';', '6', '6',
   'A', 'A', 'A', 'M', 'M', 'M', 'M', 'L', 'L', 'L', 'A', 'A', 'a', 'a', 'M',
   'M', 'm', 'm', 'L', 'L', 'B', 'B', 'a', 'a', 'a', 'm', 'm', 'm', 'l', 'l',
   'B', 'B', 'a', 'a', 'a', 'm', 'm', 'm', 'l', 'l', 'C', 'C', 'b', 'b', 'b',
   '7', '7', '7', 'l', 'l', 'C', 'C', 'b', 'b', 'b', '7', '7', '^', '^', '{',
   'C', 'c', 'c', 'c', 'c', '#', '#', '^', '^', '{', 'D', 'c', 'c', 'c', '9',
   '9', '$', '$', '^', '{', 'D', 'D', 'd', 'd', '9', '&', '&', '&', '6', '6',
   'D', 'D', 'd', 'd', 'd', ',', ',', ',', '6', '6'
};

/*
 * c1rgb - RGB values for c1 palette entries
 *
 * Entry order corresponds to c1list (above).
 * Each entry gives r,g,b in linear range 0 to 48.
 */
static unsigned char c1rgb[] = {
   0, 0, 0,		/*  0             black		*/
   8, 8, 8,		/*  1   very dark gray		*/
   16, 16, 16,		/*  2        dark gray		*/
   24, 24, 24,		/*  3             gray		*/
   32, 32, 32,		/*  4       light gray		*/
   40, 40, 40,		/*  5  very light gray		*/
   48, 48, 48,		/*  6             white		*/
   48, 24, 30,		/*  7             pink		*/
   36, 24, 48,		/*  8             violet	*/
   48, 36, 24,		/*  9  very light brown		*/
   24, 12, 0,		/*  ?             brown		*/
   8, 4, 0,		/*  !   very dark brown		*/
   16, 0, 0,		/*  n   very dark red		*/
   32, 0, 0,		/*  N        dark red		*/
   48, 0, 0,		/*  A             red		*/
   48, 16, 16,		/*  a       light red		*/
   48, 32, 32,		/*  #  very light red		*/
   30, 18, 18,		/*  @        weak red		*/
   16, 4, 0,		/*  o   very dark orange	*/
   32, 8, 0,		/*  O        dark orange	*/
   48, 12, 0,		/*  B             orange	*/
   48, 24, 16,		/*  b       light orange	*/
   48, 36, 32,		/*  $  very light orange	*/
   30, 21, 18,		/*  %        weak orange	*/
   16, 8, 0,		/*  p   very dark red-yellow	*/
   32, 16, 0,		/*  P        dark red-yellow	*/
   48, 24, 0,		/*  C             red-yellow	*/
   48, 32, 16,		/*  c       light red-yellow	*/
   48, 40, 32,		/*  &  very light red-yellow	*/
   30, 24, 18,		/*  |        weak red-yellow	*/
   16, 16, 0,		/*  q   very dark yellow	*/
   32, 32, 0,		/*  Q        dark yellow	*/
   48, 48, 0,		/*  D             yellow	*/
   48, 48, 16,		/*  d       light yellow	*/
   48, 48, 32,		/*  ,  very light yellow	*/
   30, 30, 18,		/*  .        weak yellow	*/
   8, 16, 0,		/*  r   very dark yellow-green	*/
   16, 32, 0,		/*  R        dark yellow-green	*/
   24, 48, 0,		/*  E             yellow-green	*/
   32, 48, 16,		/*  e       light yellow-green	*/
   40, 48, 32,		/*  ;  very light yellow-green	*/
   24, 30, 18,		/*  :        weak yellow-green	*/
   0, 16, 0,		/*  s   very dark green		*/
   0, 32, 0,		/*  S        dark green		*/
   0, 48, 0,		/*  F             green		*/
   16, 48, 16,		/*  f       light green		*/
   32, 48, 32,		/*  +  very light green		*/
   18, 30, 18,		/*  -        weak green		*/
   0, 16, 8,		/*  t   very dark cyan-green	*/
   0, 32, 16,		/*  T        dark cyan-green	*/
   0, 48, 24,		/*  G             cyan-green	*/
   16, 48, 32,		/*  g       light cyan-green	*/
   32, 48, 40,		/*  *  very light cyan-green	*/
   18, 30, 24,		/*  /        weak cyan-green	*/
   0, 16, 16,		/*  u   very dark cyan		*/
   0, 32, 32,		/*  U        dark cyan		*/
   0, 48, 48,		/*  H             cyan		*/
   16, 48, 48,		/*  h       light cyan		*/
   32, 48, 48,		/*  `  very light cyan		*/
   18, 30, 30,		/*  '        weak cyan		*/
   0, 8, 16,		/*  v   very dark blue-cyan	*/
   0, 16, 32,		/*  V        dark blue-cyan	*/
   0, 24, 48,		/*  I             blue-cyan	*/
   16, 32, 48,		/*  i       light blue-cyan	*/
   32, 40, 48,		/*  <  very light blue-cyan	*/
   18, 24, 30,		/*  >        weak blue-cyan	*/
   0, 0, 16,		/*  w   very dark blue		*/
   0, 0, 32,		/*  W        dark blue		*/
   0, 0, 48,		/*  J             blue		*/
   16, 16, 48,		/*  j       light blue		*/
   32, 32, 48,		/*  (  very light blue		*/
   18, 18, 30,		/*  )        weak blue		*/
   8, 0, 16,		/*  x   very dark purple	*/
   16, 0, 32,		/*  X        dark purple	*/
   24, 0, 48,		/*  K             purple	*/
   32, 16, 48,		/*  k       light purple	*/
   40, 32, 48,		/*  [  very light purple	*/
   24, 18, 30,		/*  ]        weak purple	*/
   16, 0, 16,		/*  y   very dark magenta	*/
   32, 0, 32,		/*  Y        dark magenta	*/
   48, 0, 48,		/*  L             magenta	*/
   48, 16, 48,		/*  l       light magenta	*/
   48, 32, 48,		/*  {  very light magenta	*/
   30, 18, 30,		/*  }        weak magenta	*/
   16, 0, 8,		/*  z   very dark magenta-red	*/
   32, 0, 16,		/*  Z        dark magenta-red	*/
   48, 0, 24,		/*  M             magenta-red	*/
   48, 16, 32,		/*  m       light magenta-red	*/
   48, 32, 40,		/*  ^  very light magenta-red	*/
   30, 18, 24,		/*  =        weak magenta-red	*/
   };


/*
 * palnum(s) - return palette number, or 0 if unrecognized.
 *
 *    returns +1 ... +6 for "c1" through "c6"
 *    returns +1 for NULL
 *    returns -2 ... -256 for "g2" through "g256"
 *    returns 0 for unrecognized palette name
 */
int palnum(const char *s)
   {
   char c, x;
   int n;

   if (s == NULL)
      return 1;
   if (sscanf(s, "%c%d%c", &c, &n, &x) != 2)
      return 0;
   if (c == 'c' && n >= 1 && n <= 6)
      return n;
   if (c == 'g' && n >= 2 && n <= 256)
      return -n;
   return 0;
   }

static struct palentry *palsetup_palette;	/* current palette */

/*
 * palsetup(p) - set up palette for specified palette.
 */
struct palentry *palsetup(int p)
   {
   int r, g, b, i, n, c;
   unsigned int rr, gg, bb;
   const unsigned char *s = NULL, *t;
   double m;
   struct palentry *e;
   static int palnumber;		/* current palette number */

   if (palnumber == p)
      return palsetup_palette;
   if (palsetup_palette == NULL) {
      palsetup_palette =
         (struct palentry *)malloc(256 * sizeof(struct palentry));
      if (palsetup_palette == NULL)
         return NULL;
      }
   palnumber = p;

   for (i = 0; i < 256; i++)
      palsetup_palette[i].valid = palsetup_palette[i].transpt = 0;
   palsetup_palette[TCH1].transpt = 1;
   palsetup_palette[TCH2].transpt = 1;

   if (p < 0) {				/* grayscale palette */
      n = -p;
      if (n <= 64)
         s = (unsigned char *)c4list;
      else
         s = allchars;
      m = 1.0 / (n - 1);

      for (i = 0; i < n; i++) {
         e = &palsetup_palette[*s++];
         gg = 65535 * m * i;
         e->clr.red = e->clr.green = e->clr.blue = gg;
         e->valid = 1;
	 e->transpt = 0;
         }
      return palsetup_palette;
      }

   if (p == 1) {			/* special c1 palette */
      s = (unsigned char *)c1list;
      t = c1rgb;
      while ((c = *s++) != 0) {
         e = &palsetup_palette[c];
         e->clr.red   = 65535 * (((int)*t++) / 48.0);
         e->clr.green = 65535 * (((int)*t++) / 48.0);
         e->clr.blue  = 65535 * (((int)*t++) / 48.0);
         e->valid = 1;
	 e->transpt = 0;
         }
      return palsetup_palette;
      }

   switch (p) {				/* color cube plus extra grays */
      case  2:  s = (unsigned char *)c2list;	break;	/* c2 */
      case  3:  s = (unsigned char *)c3list;	break;	/* c3 */
      case  4:  s = (unsigned char *)c4list;	break;	/* c4 */
      case  5:  s = allchars;			break;	/* c5 */
      case  6:  s = allchars;			break;	/* c6 */
      }
   m = 1.0 / (p - 1);
   for (r = 0; r < p; r++) {
      rr = 65535 * m * r;
      for (g = 0; g < p; g++) {
         gg = 65535 * m * g;
         for (b = 0; b < p; b++) {
            bb = 65535 * m * b;
            e = &palsetup_palette[*s++];
            e->clr.red = rr;
            e->clr.green = gg;
            e->clr.blue = bb;
            e->valid = 1;
	    e->transpt = 0;
            }
         }
      }
   m = 1.0 / (p * (p - 1));
   for (g = 0; g < p * (p - 1); g++)
      if (g % p != 0) {
         gg = 65535 * m * g;
         e = &palsetup_palette[*s++];
         e->clr.red = e->clr.green = e->clr.blue = gg;
         e->valid = 1;
	 e->transpt = 0;
         }
   return palsetup_palette;
   }

/*
 * rgbkey(p,r,g,b) - return pointer to key of closest color in palette number p.
 *
 * In color cubes, finds "extra" grays only if r == g == b.
 */
char *rgbkey(int p, double r, double g, double b)
   {
   int n, i;
   double m;
   char *s;

   if (p > 0) {				/* color */
      if (r == g && g == b) {
         if (p == 1)
            m = 6;
         else
            m = p * (p - 1);
         return cgrays[p - 1] + (int)(0.501 + m * g);
         }
      else {
         if (p == 1)
            n = C1Side;
         else
            n = p;
         m = n - 1;
         i = (int)(0.501 + m * r);
         i = n * i + (int)(0.501 + m * g);
         i = n * i + (int)(0.501 + m * b);
         switch(p) {
            case  1:  return c1cube + i;		/* c1 */
            case  2:  return c2list + i;		/* c2 */
            case  3:  return c3list + i;		/* c3 */
            case  4:  return c4list + i;		/* c4 */
            case  5:  return (char *)allchars + i;	/* c5 */
            case  6:  return (char *)allchars + i;	/* c6 */
            }
         }
      }
   else {				/* grayscale */
      if (p < -64)
         s = (char *)allchars;
      else
         s = c4list;
      return s + (int)(0.5 + (0.299 * r + 0.587 * g + 0.114 * b) * (-p - 1));
      }

   /*NOTREACHED*/
   return 0;  /* avoid gcc warning */
   }


/*
 * palchars(p, n) - the characters of palette p (PaletteChars), and
 * their number.
 */
const char *palchars(int p, int *n)
   {
   switch (p) {
      case 1:  *n = 90;  return c1list;
      case 2:  *n = 9;   return c2list;
      case 3:  *n = 31;  return c3list;
      case 4:  *n = 73;  return c4list;
      case 5:  *n = 141; return (const char *)allchars;
      case 6:  *n = 241; return (const char *)allchars;
      default:
         *n = -p;
         return p >= -64 ? c4list : (const char *)allchars;
      }
   }
