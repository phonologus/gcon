# Graphics

The display server is a separate process (`libexec/gcon-graphics`) that
the runtime starts when a program first opens a window, and talks to
through [a protocol](internals/protocol.md). There are five.
`./build.sh` builds the platform's own: on macOS
[the Cocoa server](#the-cocoa-display-server); elsewhere
[the Wayland server](#the-wayland-display-server) and
[the X11 server](#the-x11-display-server), each where its libraries are
installed, the Wayland one first; otherwise none. `./build.sh
--withCocoa`, `--withWayland`, `--withX11` or `--withSDL` names the
servers to build instead, the first being the one gcon-vm runs:
`--withX11` on macOS (under XQuartz) for Arizona Icon's exact pictures,
say, or `--withSDL` for [the SDL server](#the-sdl-display-server), the
portable one, which needs the system's SDL3 and is built only when
named. Each is also installed as `libexec/gcon-graphics-NAME` (`cocoa`,
`wayland`, `x11`, `sdl`), and `GCONGRAPHICS` chooses another at run
time: anything that speaks the protocol. Where the default server finds
no display, gcon-vm tries the X11 one, if it was built: so the Wayland
server's programs open their windows on X in an X session, or over
`ssh -X`.

The fifth is always built: [the headless server](#the-headless-display-server),
which has no display. gcon-vm runs it where no server has a display
(no X display and no Wayland compositor, say) or there is no default
server, so hidden canvases (drawing, `Pixel`, `WriteImage`)
work anywhere: a filter can make pictures on a machine with no screen.
A window cannot be opened there: `open` fails, as Icon's does without a
display, and a program that goes on to draw on `&window` meets run-time
error 140. A server `GCONGRAPHICS` names is used as it is, without this
fallback.

Whichever server draws:

- **The preprocessor defines `_SDL_GRAPHICS`, not `_X_WINDOW_SYSTEM`**,
  and `&features` says `SDL graphics`, so programs that test for X take
  their portable path. Keys are still reported as X keysyms: Gcon's
  copy of the library carries two small changes, the same as icon-sdl's
  (made by `tools/ipl/patch.sh`), so that `keysyms.icn`'s key names
  work.
- **Image files:** GIF, as in Icon, and PNG, JPEG, BMP and QOI, chosen by
  the file's extension for `WriteImage` (any other is written as GIF)
  and by its contents for `ReadImage`. gcon-vm reads and writes them
  itself, so every server has them all.
- **Antialiasing** is Gcon's `antialias` attribute, a context's like
  `linestyle`: `on` by default, it smooths the edges of shapes and text
  drawn in copy mode without a pattern, where the server can (the
  Cocoa, SDL and headless servers; the X11 server draws as X does,
  aliased). `WAttrib("antialias=off")` gives X's covered-or-not pixels,
  and `GCONANTIALIAS=off` makes that the default for every program. The
  other drawops (`drawop=reverse` among them) and patterns are always
  drawn as X draws them, so rubber-banding and stippled fills keep their
  meaning.
- **Not supported:** the numeric colour forms `rgb:r/g/b` and `RGBi:...`.
  `iconlabel=` is accepted but never shown, since modern desktops have
  no icon labels.

A program can also be made without graphics, wherever it runs: `gcon
build -nographics` (or `gcon run -nographics`) translates it as Icon
built without graphics. `_GRAPHICS` is not defined, the graphics
functions do not exist (calling one is run-time error 106), linking the
graphics library (`link graphics`) is an error when the program is
translated, `open` fails for a window, and `&window`, `&x`, `&y` and
the other graphics keywords fail. It never starts a display server: for
a filter, or a server.

## The Cocoa display server

`gcon-graphics-cocoa` draws with the Mac's own graphics: AppKit's
windows, Core Graphics and CoreText. Lines, shapes and text are
antialiased and drawn at the display's scale (Retina), so it looks like
a Mac program rather than an X one; Gcon's reference tests allow the
edges of slanted lines and curves, and text (its fonts are the Mac's),
to differ from Arizona Icon's X11 pictures, and nothing else.

- **Drawing in copy mode** (the default `drawop=copy`, no pattern) is
  Core Graphics's, antialiased unless `antialias` is `off`. **Every
  other drawop, `drawop=reverse` and patterns** are drawn as X draws
  them, aliased and combined pixel by pixel.
- **Fonts** are the same as the SDL server's on macOS, through CoreText:
  `mono` is Menlo, `sans` Helvetica, `serif` Times and `typewriter`
  Courier, and a font name that isn't found fails, as under X.
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
- **No HiDPI scaling, no antialiasing:** one Icon pixel is one pixel,
  and X's core drawing is aliased, as in X (`antialias` has no effect).

## The Wayland display server

`gcon-graphics-wayland` is a native Wayland client. Wayland has no
drawing of its own, only buffers a client fills, so it draws with
Gcon's own rasterizer, as [the SDL server](#the-sdl-display-server)
does: the same pictures, antialiased (`antialias`), at the display's
scale. It needs libwayland-client, xkbcommon and wayland-cursor, and
uses libdecor where it was found when built.

- **`WAYLAND_DISPLAY`** names the compositor, as for any Wayland
  client. Without one it reports no display, and gcon-vm tries the X11
  server instead, if it was built.
- **Title bars** are the compositor's where it draws them (KDE, sway),
  else libdecor's (GNOME's compositor draws none); `GCONLIBDECOR=off`
  leaves them to the compositor alone.
- **Fonts** are fontconfig's, their outlines drawn by stb_truetype, as
  the SDL server's are off macOS: `sans` is the system's sans-serif
  (DejaVu Sans, say), `serif` and `mono` likewise.
- **Keys** come through the compositor's keymap by xkbcommon, as X
  keysyms; they repeat at the compositor's rate.
- **What Wayland withholds:** a client cannot place its window, learn
  where it is, raise or lower it, move the pointer or read it outside
  its windows. So reading `pos`, `posx` or `posy` fails, and setting
  them does nothing; `Raise` and `Lower` do nothing, nor does
  `pointerx=`; and minimizing (`canvas=iconic`) is a request the
  compositor may ignore. `Alert` makes no sound: Wayland has no bell.

## The SDL display server

`gcon-graphics-sdl`, built by `./build.sh --withSDL`, opens its windows
with the system's SDL3, and draws with Gcon's own rasterizer and fonts,
as the headless server does: it needs SDL3's development files (and
fontconfig's off macOS), and nothing else. It is not an X11 client. It
began as the backend of [icon-sdl](https://github.com/phonologus/icon-sdl),
a fork of Icon 9.5 whose iconx drew with SDL3 and Blend2D.

- **HiDPI and antialiased:** one Icon pixel is one point, drawn at the
  display's scale (`ICONSDL_SCALE` overrides it), smoothed (`antialias`).
  With `antialias=off`, lines, fills, patterns, images and all 16
  drawops follow X's rules: Arizona Icon's X11 pictures but for a few
  pixels at the edges of circles and arcs.
- **Fonts are the system's own,** found through CoreText on macOS and
  fontconfig on Linux, their TrueType and CFF outlines drawn by
  stb_truetype, kerned. The portable families map to them: on macOS,
  `mono` is Menlo, `sans` Helvetica, `serif` Times and `typewriter`
  Courier. Metrics are close to X's but not identical. A font name that
  isn't found fails, as under X; X's misc-fixed fonts (`6x13`, `7x13B`)
  are there by their own names.
- **Keys:** on macOS, Option is `&meta`; `ICONSDL_META=command` makes it
  the Command key. Keypad keys report the characters they type, not
  keypad keysyms.

## The headless display server

`gcon-graphics-headless` has no display and no windows, only hidden
canvases. It needs a C compiler alone, and draws with Gcon's own
rasterizer, antialiased (`antialias`); with `antialias=off`, by X's
rules, its pictures are Arizona Icon's X11 ones but for a few pixels at
the edges of circles and arcs (X computes arcs exactly; it flattens
them), and for text.

- **Fonts are X's misc-fixed bitmap fonts:** `fixed` is X's 6x13, and
  `mono`, `typewriter`, `sans` and `serif` all take the misc-fixed font
  nearest the size, with its bold and oblique faces; a font's own name
  (`7x13B`) works too.
- **No HiDPI scaling:** one Icon pixel is one pixel.
