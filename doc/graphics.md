# Graphics

The display server is a separate process (`libexec/gcon-graphics`) that
the runtime starts when a program first opens a window, and talks to
through [a protocol](internals/protocol.md). There is more than one:
`./build.sh` builds the SDL3 + Blend2D server, the portable one, by
default; `./build.sh --withX11` builds [the X11 server](#the-x11-display-server)
instead, and `--withSDL --withX11` both, the first named being the one
gcon-vm runs (each is also installed as `libexec/gcon-graphics-sdl` or
`-x11`, and `GCONGRAPHICS` chooses another at run time). A Cocoa server
for macOS is planned.

The SDL server is not an X11 client. The backend was first written for
Arizona Icon itself, in
[icon-sdl](https://github.com/phonologus/icon-sdl), a fork of Icon 9.5
whose iconx draws with it; Gcon's display server is that backend in a
process of its own.

- **The preprocessor defines `_SDL_GRAPHICS`, not `_X_WINDOW_SYSTEM`**,
  and `&features` says `SDL graphics`, so programs that test for X take
  their portable path. Keys are still reported as X keysyms: Gcon's
  copy of the library carries two small changes, the same as icon-sdl's
  (made by `tools/ipl/patch.sh`), so that `keysyms.icn`'s key names
  work.
- **Drawing is antialiased and HiDPI.** One Icon pixel is one point,
  drawn at the display's scale; `ICONSDL_SCALE` overrides it. Axis-aligned
  lines, fills, patterns, images and all 16 drawops match Arizona Icon's
  X11 output exactly; the edges of circles, slanted lines and thin arcs
  differ by a few pixels.
- **Fonts are the system's own,** found through CoreText on macOS and
  fontconfig on Linux. The portable families map to them: on macOS,
  `mono` is Menlo, `sans` Helvetica, `serif` Times and `typewriter`
  Courier. Metrics are close to X's but not identical. A font name that
  isn't found fails, as under X.
- **Image files:** GIF, as in Icon, and also PNG, JPEG, BMP and QOI, for
  both `ReadImage` and `WriteImage`.
- **Keys:** on macOS, Option is `&meta`; `ICONSDL_META=command` makes it
  the Command key.
- **Not supported:** the numeric colour forms `rgb:r/g/b` and `RGBi:...`.
  `iconlabel=` is accepted but never shown, since modern desktops have
  no icon labels. Keypad keys report the characters they type, not
  keypad keysyms.

Without the display server (it needs a C++ compiler and CMake to
build), every other part of Gcon works; a program that opens a window
stops with a message that the server cannot be run. `GCONGRAPHICS`
names another display server: anything that speaks
[the protocol](internals/protocol.md).

## The X11 display server

`gcon-graphics-x11` draws with X's own requests, as Arizona Icon's X11
backend does, so its pictures are Arizona's: Gcon's reference tests
match Arizona's X11 images exactly, but for text. It needs only Xlib,
and runs wherever there is an X display: Linux and the BSDs, Wayland
through XWayland, and macOS through XQuartz.

- **`DISPLAY`** names the X display, as for any X client.
  `WAttrib(w, "display")` reports it, and `display=` may name it when a
  window is opened (the IPL's dialogs pass it on).
- **Fonts are X's core fonts,** chosen as Arizona Icon chooses them:
  `mono` is Lucida Typewriter, `typewriter` Courier, `sans` Helvetica and
  `serif` Times, and the default is X's `fixed`. A full X font name
  works too.
- **No HiDPI scaling:** one Icon pixel is one pixel, as in X.
- **Image files:** GIF only, for now; PNG, JPEG, BMP and QOI need the
  SDL server.
- As with the SDL server, the preprocessor defines `_SDL_GRAPHICS` and
  `&features` says `SDL graphics`: what a program can do is the same
  whichever server draws it.
