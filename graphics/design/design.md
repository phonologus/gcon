# gcon-graphics design notes

## Role

gcon-graphics is Gcon's display server. The decision and its reasons are
in Gcon's `design/design.md` ("Graphics: a separate display server"). In
brief: gcon-vm owns Icon's graphics semantics (the event queue as an Icon
list, `Event()`, polling, `&x`, `&y`, `&interval`, `WDelay`, window
and context values); the server owns windows, canvases, drawing and
raw events. Canvases live in the server, as in X11 and Plan 9's
`/dev/draw`, so `Pixel`, `CopyArea` and `drawop=reverse` work on
the server's pixels.

## Origin and what changes

Forked from icon-sdl (`sdl-graphics` at 3b5a888). There the code was a
library linked into iconx: `sdlgfx.h` is its plain-C interface to the
interpreter (the comments in `src/` still describe it that way), and
`plat.c`'s `pl_main` ran the interpreter on a second thread so SDL
could have the main thread. Here the "interpreter" thread becomes the
protocol reader: it decodes requests and calls the canvas and platform
functions, much as `rsdlwin.ri` did in iconx. The main-thread request
mechanism carries over unchanged.

The canvas layer stays as it is: its behaviour was compared with Icon's
X11 backend (icon-sdl's `sdl/tests/ref/`), and the protocol should not
need it to change.

## Protocol

Modelled on Plan 9's draw protocol (draw(3)): compact binary messages,
objects named by integer ids chosen by the client, drawing requests that
need no reply, and few messages that do. Rules carried from Gcon's
design:

- drawing requests are pipelined; gcon-vm flushes when it blocks, flushes
  explicitly, needs a reply, or exits;
- replies only where an answer is needed: `Pixel` (one per rectangle),
  opening windows and fonts, geometry queries;
- nothing that can fail in Icon terms is left to an asynchronous error;
  a failing pipelined request is a protocol error;
- font metrics and glyph advances come back when a font opens, so text
  width needs no round trip;
- image files are read and written by gcon-vm, which does GIF itself;
  the server encodes and decodes the other formats in memory (`ENCODE`,
  `DECODE`), and never opens the program's files;
- windows (canvases) and graphics contexts are separate objects, so
  `Clone` and `Couple` map onto them as onto X11's drawables and GCs;
- a version handshake comes first;
- events flow back unsolicited, in the X conventions `plat.c` already
  produces.

The transport is a socketpair inherited from gcon-vm (the server is its
child), or a named socket.

## Done (2026-11-26)

1. The protocol specification (`doc/internals/protocol.md` in Gcon's tree, version 2; events
   are fetched by the client with `WAIT` rather than sent unsolicited:
   see below), with fonts, text, window requests, icons and image
   files.
2. The protocol reader here, replacing `pl_main`'s interpreter thread.
3. icon-sdl's reference and window tests, driving the server through
   gcon-vm (`../vm/tests/ref.sh`, `window.sh`). The planned headless
   Go server for Gcon's tests went with Gcon's Go: hidden canvases
   (`../vm/tests/graphics.sh`) need no display.

## Events: fetched, not pushed (2026-11-26)

The plan was for events to flow back unsolicited. Version 1 has the
client fetch them instead: `WAIT(timeout)` shows what has changed,
waits for an event or the time-out, and replies with the events that
have arrived. Icon's model needs nothing more: the client wants events
only when the program asks for them (`Event`, `Pending`) or when it
polls while drawing, and in each case it is the client that is waiting.
So the reader stays one thread that only ever answers, the client never
has to read the socket asynchronously, and the order of replies is
simply the order of requests. Pushed events can be added later (a
second message direction) if a client needs them.

## Native display servers (Sean, 2026-11-28)

With the protocol in place, a display server no longer has to be
portable: each platform can have a light server of its own, built on
the platform's graphics, and `./build.sh --withX11`, `--withCocoa` and
so on choose which are built. The SDL3 + Blend2D server stays, as the
portable one (and the default) and as an oracle, until the native ones
have earned trust.

- **X11 first** (`x11/`): Xlib only, plain C. The protocol is X's
  model already (the GC function as drawop, X's line and fill styles,
  `XDrawImageString` for `write`, Icon's arcs), so the server is close
  to a transliteration, and its pixels are X's own: `vm/tests/ref.sh`
  compares with Arizona Icon's X11 images, which this server should
  match exactly but for fonts. It also runs under XWayland, and under
  XQuartz (and its `Xvfb`, for headless tests) on macOS.
- **Cocoa next**, drawing natively with Core Graphics and CoreText:
  graphics are the Mac's strong suit, so the Mac's server exploits it
  (antialiased, its own fonts) rather than imitating X, and X11 and
  Wayland are second-class in image quality.
- **Wayland later**: Wayland has no drawing API, only buffers, so a
  native Wayland server needs a rasterizer of its own (X's core
  semantics in C, drawing into shared memory).
- The native servers reuse `src/main.c` (the protocol reader) and
  `src/xcolors.c`, and supply the three layers of `src/sdlgfx.h`
  (`cv_*` canvases, `pl_*` windows and events, `ft_*` fonts), as the
  SDL server does with `canvas.c`, `plat.c` and the font files. The
  interface is already single-threaded underneath: only SDL's `pl_main`
  needs a second thread.
- Gcon's rule of libc only becomes, for display servers, libc and the
  platform's own graphics libraries, found when built (Xlib; AppKit,
  Core Graphics and CoreText). Nothing is vendored for them.
- Image files: PNG, JPEG, BMP and QOI are the server's (`ENCODE`,
  `DECODE`), and X11 has no codecs; until they move into gcon-vm (a
  vendored public-domain codec in C), the X11 server answers that it
  writes no such format and reads none. GIF, Icon's own, is the
  client's already.
- Icon 9.5's `Win` functions (`WinButton`, `WinOpenDialog`, ...; MS
  Windows only in 9.5, and used by nothing in Icon's tree, the IPL or
  `../icn`) are for after the servers, on Cocoa first: dialogs map well
  onto AppKit, while X11 and Wayland have no toolkit of their own.

### The Cocoa server (2026-11-28)

`cocoa/`: `ccanvas.c` and `cfont.c` in C (Core Graphics, CoreText,
ImageIO), `cplat.m` the only Objective-C (AppKit). As the SDL server,
the protocol reader runs on a thread of its own and AppKit has the main
thread: window requests run there (`dispatch_sync`), and events come
back through a queue the reader waits on. A canvas is a bitmap context
over pixels laid out as the SDL server's (0xFFRRGGBB, at the display's
backing scale for a window, 1 for a hidden canvas), so `canvas.c`'s
pixel operations carry over: copy, read, write, erase and the X GC
functions.

- Copy-mode drawing without a pattern, nearly all drawing, is Core
  Graphics's own, antialiased, and text CoreText's; any other drawop,
  `drawop=reverse` and patterns are drawn aliased into an A8 mask and
  combined pixel by pixel, as X does, so rubber-banding and stipples
  keep Icon's meaning. Against Arizona's X11 images
  (`vm/tests/ref/tolerance-cocoa`): bounds, colours, drawops, images and
  patterns exact; lines, shapes and clipcopy differ at antialiased
  edges (about 3%, 3% and 14% of pixels; clipcopy is dense with slanted
  lines); text in its fonts.
- A window shows its canvas as the contents of a layer it hosts: an
  image of the canvas, made on each showing that follows a change. The
  text cursor is a sublayer, removed by the next showing.
- Fonts and their metrics are the SDL server's on macOS (the same
  families, through CoreText), so `vm/tests/graphics.std` holds.
- Images through ImageIO: PNG, JPEG, BMP (and reading what ImageIO
  reads); QOI is not among them.
- The first window makes the process a regular application (a Dock
  icon, a menu with Quit); hidden canvases alone do not. Command-Q is a
  close event for the key window, which the program decides on.
- An expose event follows a window's first showing: gcon-vm reports
  resizes only for a window that has been exposed.

### What ./build.sh builds (Sean, 2026-11-28)

The native servers are the default, the Mac's quality first, and the
SDL server is opt-in: heavyweight, and the only part of Gcon that needs
C++ and CMake. With no flags `./build.sh` probes for the platform's own
server: Cocoa on macOS (`cocoa/build.sh -check`), else X11 where Xlib
is found (`x11/build.sh -check`), else none, and Gcon is built without
graphics (an earlier build's `libexec/gcon-graphics` is removed, so
none runs by mistake). The SDL server is built only by `--withSDL`,
never alongside a native one unasked; where Arizona's exact pictures
matter on a Mac, `--withX11` builds the X11 server, for XQuartz. With
flags, exactly the servers named are built, the first being the
default.

The tests follow the server installed: `vm/tests/graphics.sh`, `ref.sh`
and `window.sh` test `libexec/gcon-graphics` (or `$GCONGRAPHICS`), and
choose the expected output and tolerances by its `-version` (X11:
`graphics-x11.std`, `ref/tolerance-x11`; Cocoa: `ref/tolerance-cocoa`;
SDL: the originals); without a server they say so and check nothing.
`iconx.sh` compares with icon-sdl's iconx, so it wants the SDL server
(`graphics/bin/gcon-graphics`) and skips without it.

### Image files move into gcon-vm; protocol version 3 (2026-11-28)

PNG, JPEG, BMP and QOI were the server's (`ENCODE`, `DECODE`): the SDL
server's through Blend2D, the Cocoa server's through ImageIO (no QOI),
and the X11 server had none. They are gcon-vm's now
(`vm/src/imgcodec.c`), as GIF always was: stb_image and
stb_image_write (vendored in `deps/stb`, pinned in `deps/deps.lock`,
public domain) for PNG, JPEG and BMP, and QOI written from its
specification. So every server has every format, and a server only
draws: the protocol is version 3, without `ENCODE` and `DECODE` (36 and
37 reserved), and the three servers lost their codec code (the Cocoa
server no longer links ImageIO).

- `ReadImage` tries GIF (Icon's reader), then decodes by the file's
  contents; alpha below half is transparent, as before, now straight
  rather than premultiplied (Icon's pixels have no alpha to blend).
- `WriteImage` chooses by the extension, as before: `png`, `jpg` or
  `jpeg`, `bmp`, `qoi` (any case), and any other as GIF. JPEG at
  quality 95, above stb's threshold for chroma subsampling, which
  smeared colour across the sharp edges Icon draws (a round trip of
  coloured rectangles differed by up to 172/256 at 90, 29/256 at 95).
- `vm/tests/graphics.icn` round-trips each format under each server:
  the lossless exactly; damaged files (truncated, bytes flipped) are
  refused or decoded without fault under ASan and UBSan.

### Our own rasterizer: the spike (2026-11-28)

Cocoa draws with Core Graphics, and X11 with X; Blend2D is in the SDL
server alone, which uses it only to fill coverage masks (the X way of
combining pixels, `canvas.c`'s `apply`, is our own C) and for text. So
a rasterizer of our own, in plain C, replaces Blend2D's mask filling:
for a Wayland server (Wayland has no drawing), for a headless server
(hidden canvases where there is no display: the X11 server needs an X
display even for those), and perhaps one day for the SDL server.

`raster/raster.c` (about 300 lines): a scanline filler through pixel
centres with half-open spans, so a centre on a top or left edge is in
and one on a bottom or right edge out (X's rule, with no tie-breaking
nudge); even-odd or nonzero. Strokes are the union of convex pieces,
each filled alone: a rectangle per segment, a mitre (or, under X's 11
degrees, a bevel) per join, projecting squares for square caps. Arcs
are flattened to polygons (finely enough for the scale), dashes cut
along the path as `canvas.c` cuts them.

`headless/` is the first server on it: `main.c`, `hcanvas.c` (the Cocoa
server's pixel layer with every shape through `raster.c`) and `hplat.c`
(no windows: opening one fails; no events). 88 KB, a C compiler alone.
Text is a placeholder: any font opens with `fixed`'s metrics and draws
nothing.

Against Arizona's X11 images (`vm/tests/ref.sh`), first time: bounds,
colours, drawops, images and patterns exact; clipcopy 2 pixels, lines
3, shapes 13 (the edges of a filled circle and a pie: X rasterizes arcs
exactly, we flatten them), against the SDL server's allowances of 30,
10 and 60; text differs, having no glyphs. Clean under ASan and UBSan.
`vm/tests/graphics.sh` differs only in font metrics.

Not wired into `./build.sh` yet. Open: text (bitmap fonts first: X's
public-domain `misc-fixed`, embedded; scalable ones would need a font
rasterizer and a font to ship), whether the headless server becomes the
fallback where there is no display, and the Wayland server.

### Text in the headless server: X's misc-fixed (2026-11-28)

Bitmap fonts first: X's misc-fixed, the fonts behind X's own `fixed`,
in the public domain. `deps/update.sh fixed [COMMIT]` fetches them from
xorg/font/misc-misc and keeps their Latin-1 glyphs (some 190 of each
font's 4000), still as BDF, in `deps/fixed` (18 fonts, 5x8 to 10x20
with the bold and oblique faces, 496 KB), pinned in `deps/deps.lock`.
`raster/bdf2c.sh` (awk) turns them into C tables when the server is
built; nothing generated is committed. The server is 221 KB with them.

`fixed` is 6x13, as X's; Icon's portable families take the nearest
size, with the B and O faces for bold and italic; a font's own name
(`7x13B`) opens it, as an X font name would; any other name fails.
Glyphs go through the mask like shapes, so drawops and patterns apply;
image text (`write` to a canvas) is X's `XDrawImageString`, the cell
filled with bg and the glyphs in fg, in copy mode whatever the drawop.

Against the reference, `fixed` has Arizona's metrics exactly; the other
families are Arizona's Lucida, Courier, Helvetica and Times there, so
text differs, and `graphics.sh` has its own expected output
(`graphics-headless.std`: the X11 server's but for two font lines).
`vm/tests/headless.sh` runs both; the tolerances
(`ref/tolerance-headless`) allow a few pixels at arc edges and any text.
