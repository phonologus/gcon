/*
 * hplat.c -- the headless display server's platform layer: no display,
 * so no windows (opening one fails) and no events; hidden canvases
 * are all it has (its fonts are hfont.c's). MIT licence, see ../LICENSE.md.
 */
#include <poll.h>
#include <stdlib.h>
#include "sdlgfx.h"

int pl_main(int (*f)(int, char **), int argc, char **argv) { return f(argc, argv); }
void pl_shutdown(void) {}

void *pl_open(const char *title, int x, int y, int w, int h, int state, int resizable, int *scale)
{
   (void)title; (void)x; (void)y; (void)w; (void)h; (void)state; (void)resizable;
   *scale = 1;
   return NULL;			/* no display: no windows */
}

void pl_close(void *win) { (void)win; }
void pl_show(void *win) { (void)win; }
void pl_present(void *win, void *cv, int lw, int lh, int full) { (void)win; (void)cv; (void)lw; (void)lh; (void)full; }
void pl_title(void *win, const char *s) { (void)win; (void)s; }
void pl_geometry(void *win, int x, int y, int w, int h) { (void)win; (void)x; (void)y; (void)w; (void)h; }
int pl_position(void *win, int *x, int *y) { (void)win; *x = *y = 0; return 0; }
void pl_state(void *win, int state) { (void)win; (void)state; }
void pl_resizable(void *win, int on) { (void)win; (void)on; }
void pl_raise(void *win) { (void)win; }
void pl_lower(void *win) { (void)win; }
void pl_cursor(void *win, int cursor) { (void)win; (void)cursor; }
void pl_warp(void *win, int x, int y) { (void)win; (void)x; (void)y; }
void pl_beep(int volume) { (void)volume; }
void pl_icon(void *win, const unsigned int *px, int w, int h) { (void)win; (void)px; (void)w; (void)h; }
void pl_overlay(void *win, int x, int y, int w, int h, unsigned int rgb, int scale)
{
   (void)win; (void)x; (void)y; (void)w; (void)h; (void)rgb; (void)scale;
}
void pl_display_size(void *win, int *w, int *h) { (void)win; *w = *h = 0; }
int pl_global_mouse(int *x, int *y) { *x = *y = 0; return 0; }
int pl_poll(pl_event *e) { (void)e; return 0; }

int pl_wait(int timeout_ms)
{
   poll(NULL, 0, timeout_ms < 0 ? 100 : timeout_ms);
   return 0;
}

const char *gfx_describe(void) { return "headless (gcon's rasterizer)"; }


int pl_display(void) { return 0; }
