/*
 * main.c -- gcon-graphics, the display server for gcon.
 * MIT licence, see LICENSE.md.
 *
 * Not yet a server: the protocol is still to be designed (see
 * docs/design.md). For now it checks that SDL3 and Blend2D are linked
 * and working.
 */
#include <stdio.h>
#include <string.h>
#include "sdlgfx.h"

int main(int argc, char **argv)
{
   int v;

   if (argc == 2 && strcmp(argv[1], "-version") == 0) {
      v = sdlgfx_probe();
      if (v == 0) {
         fprintf(stderr, "gcon-graphics: SDL3 or Blend2D not working\n");
         return 1;
      }
      printf("gcon-graphics (protocol not yet implemented); SDL %d.%d.%d\n",
         v / 10000, v / 100 % 100, v % 100);
      return 0;
   }
   fprintf(stderr, "usage: gcon-graphics -version\n"
      "(the gcon protocol is not implemented yet)\n");
   return 2;
}
