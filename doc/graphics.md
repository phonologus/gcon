# Graphics

The display server is an SDL3 + Blend2D backend, run as a separate
process (`libexec/gcon-graphics`) that the runtime starts when a program
first opens a window, and talks to through
[a protocol](internals/protocol.md). It is not an X11 client. The
backend was first written for Arizona Icon itself, in
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
