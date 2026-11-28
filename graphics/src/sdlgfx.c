/*
 * sdlgfx.c -- the SDL server's version (the system's SDL3).
 * (icon-sdl fork; MIT licence, see LICENSE.md.)
 */
#include <stdio.h>
#include <SDL3/SDL.h>
#include "sdlgfx.h"

int sdlgfx_probe(void)
{
   int v = SDL_GetVersion();
   return SDL_VERSIONNUM_MAJOR(v) * 10000 + SDL_VERSIONNUM_MINOR(v) * 100
      + SDL_VERSIONNUM_MICRO(v);
}

const char *gfx_describe(void)
{
   static char s[48];
   int v = sdlgfx_probe();
   snprintf(s, sizeof s, "SDL %d.%d.%d (gcon's rasterizer)", v / 10000, v / 100 % 100, v % 100);
   return s;
}
