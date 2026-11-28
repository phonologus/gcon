# Graphics

The display server is a separate process (`libexec/gcon-graphics`) that
the runtime starts when a program first opens a window, and talks to
through [a protocol](internals/protocol.md). There are three.
`./build.sh` builds the platform's own: on macOS
[the Cocoa server](#the-cocoa-display-server), elsewhere
[the X11 server](#the-x11-display-server) where Xlib is installed, and
otherwise none. `./build.sh --withCocoa`, `--withX11` or `--withSDL`
names the servers to build instead, the first being the one gcon-vm
runs: `--withX11` on macOS (under XQuartz) for Arizona Icon's exact
pictures, say, or `--withSDL` for [the SDL server](#the-sdl-display-server),
the portable one, which needs C++ and CMake and is built only when
named. Each is also installed as `libexec/gcon-graphics-NAME` (`cocoa`,
`x11`, `sdl`), and `GCONGRAPHICS` chooses another at run time: anything
that speaks the protocol.

Whichever server draws:

- **The preprocessor defines `_SDL_GRAPHICS`, not `_X_WINDOW_SYSTEM`**,
  and `&features` says `SDL graphics`, so programs that test for X take
  their portable path. Keys are still reported as X keysyms: Gcon's
  copy of the library carries two small changes, the same as icon-sdl's
  (made by `tools/ipl/patch.sh`), so that `keysyms.icn`'s key names
  work.
- **Not supported:** the numeric colour forms `rgb:r/g/b` and `RGBi:...`.
  `iconlabel=` is accepted but never shown, since modern desktops have
  no icon labels.

Without a display server, every other part of Gcon works; a program
that opens a window stops with a message that the server cannot be
run.

## The Cocoa display server

`gcon-graphics-cocoa` draws with the Mac's own graphics: AppKit's
windows, Core Graphics and CoreText. Lines, shapes and text are
antialiased and drawn at the display's scale (Retina), so it looks like
a Mac program rather than an X one; Gcon's reference tests allow the
edges of slanted lines and curves, and text (its fonts are the Mac's),
to differ from Arizona Icon's X11 pictures, and nothing else.

- **Drawing in copy mode** (the default `drawop=copy`, no pattern) is
  Core Graphics's, antialiased. **Every other drawop, `drawop=reverse`
  and patterns** are drawn as X draws them, aliased and combined pixel
  by pixel, so rubber-banding and stippled fills work exactly as in Icon.
- **Fonts** are the same as the SDL server's on macOS, through CoreText:
  `mono` is Menlo, `sans` Helvetica, `serif` Times and `typewriter`
  Courier, and a font name that isn't found fails, as under X.
- **Image files:** GIF, PNG, JPEG and BMP (QOI needs the SDL server).
- **Keys and the menu:** Option is `&meta` (`ICONSDL_META=command` makes
  it Command); Command-Q is the same as closing the window.
  `ICONSDL_SCALE` overrides the display's scale.

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
- **Image files:** GIF only, for now; PNG, JPEG and BMP need the Cocoa
  or SDL server, QOI the SDL one.

## The SDL display server

`gcon-graphics-sdl`, built by `./build.sh --withSDL`, draws with SDL3
and Blend2D, vendored in the checkout (so it needs a C++ compiler and
CMake to build, and is heavyweight). It is not an X11 client. The
backend was first written for Arizona Icon itself, in
[icon-sdl](https://github.com/phonologus/icon-sdl), a fork of Icon 9.5
whose iconx draws with it; Gcon's SDL server is that backend in a
process of its own.

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
  the Command key. Keypad keys report the characters they type, not
  keypad keysyms.
