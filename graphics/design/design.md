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
  `../icn`) are parked, perhaps out of scope (Sean, 2026-11-28): they
  stray into toolkit territory, and X11 and Wayland have no toolkit of
  their own.

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

### The headless server everywhere, as the fallback (Sean, 2026-11-28)

`./build.sh` always builds the headless server (a C compiler is all it
needs) as `libexec/gcon-graphics-headless`, never the default. gcon-vm
runs it where its default server has no display, or there is none, so
hidden canvases work on any machine: a filter that draws and writes
PNGs needs no screen. The question was whether a program that expects
a display should be left running: it is not, any more than under
Arizona without an X display, because a window still cannot be opened
there: `open` fails (a careful program stops with its own message), and
drawing on `&window` after it is run-time error 140. Only hidden
canvases change: they work instead of failing.

To know in time, before any canvas exists (a program's canvases cannot
be split between two servers: `CopyArea` would break), the protocol's
`HELLO` reply says whether the server has a display (version 4):
`pl_display` in each platform layer (X11: its display opens; Cocoa: a
screen; SDL: on macOS always, elsewhere `DISPLAY` or `WAYLAND_DISPLAY`,
without starting SDL, which would give a hidden canvas a Dock icon;
headless: never). gcon-vm (`gp_connect`) starts `$GCONGRAPHICS` as it
is (an explicit choice, and what the tests use); else the
`gcon-graphics` beside it, replaced by `gcon-graphics-headless` if it
reports no display; else the headless server if there is no default;
else `gcon-graphics` on `$PATH`. `vm/tests/headless.sh` checks the
fallback both ways (no default; the X11 server without `DISPLAY`).

### Scalable fonts: stb_truetype (Sean, 2026-11-28)

What matters (Sean): TrueType and CFF outlines, antialiased; Gcon is a
language with graphics, not a graphics engine, so no typographic layout
(no OpenType GSUB features, no shaping) and no hinting. FreeType was
weighed (every outline format: CFF2, Type 1, variable fonts; hinting;
fuzzed) and found too heavy; stb_truetype (deps/stb, one public-domain
file) reads TrueType and CFF (OpenType "OTTO") outlines, with kerning
from the kern table and GPOS pairs, and rasterizes them antialiased. It
does not read CFF2, variable fonts or Type 1, and is not hardened
against malformed fonts (the fonts are the system's).

`raster/rfont.c`: the misc-fixed bitmap fonts, and, where the server can
find the platform's fonts (`RFONT_LOOKUP`, with `../src/fontmac.c`'s
CoreText or `fontlinux.c`'s fontconfig, which give a file and a
PostScript name), outline fonts by stb_truetype: the face in a
collection by its PostScript name, the size the em in pixels, ascent
and descent the hhea table's rounded up, the maximum width over the
printable Latin-1 characters; a string's width its advances and kerning
rounded once; glyphs rendered at the canvas's scale, cached. Their
coverage goes into the mask, so text follows the gc's antialias as
shapes do. With a lookup every name goes to it, as the SDL server's
fonts did; without one (the headless server, which stays free of it and
of stb) the misc-fixed policy holds. A server built with the lookup on
macOS gives the SDL server's `graphics.std` exactly (the same fonts,
the same metrics, through stb instead of Blend2D), and renders a CFF
font (Source Sans 3) cleanly, kerned.

### The SDL server without Blend2D, on the system's SDL3 (Sean, 2026-11-28)

The SDL server now draws with `raster/` (`rcanvas.c`, with dirty
tracking and `cv_pixels` for `plat.c`, which shows the canvas as it
did; `rfont.c` with the lookup, `fontmac.c` or `fontlinux.c`), and
links the system's SDL3 (pkg-config's `sdl3`; fontconfig off macOS;
AppKit on macOS for `plat.c`'s `NSBeep`, which the static SDL used to
bring). Blend2D, AsmJit, the vendored SDL3 tarball, `CMakeLists.txt`,
`graphics/deps` and `canvas.c` are gone: no C++, no CMake. 4.2 MB to
281 KB. `--withSDL` needs SDL3's development files (`brew install
sdl3`; `linux-packages.txt`, `linux-packages-fedora.txt`).

Its pictures are the headless server's (aliased until the antialias
attribute): `ref.sh` gives 2, 3 and 13 pixels where Blend2D needed 30,
10 and 60 (`ref/tolerance` tightened to 6, 6, 20), and `graphics.std`
holds, the fonts and metrics the same. `iconx.sh` no longer requires
icon-sdl's pixels (Blend2D's): output, exit status and canvases must
match, and pixel differences are reported; and it knows colrpick,
where gcon follows Arizona's `display=` and icon-sdl does not.

### Arizona, not icon-sdl, as the oracle for graphics programs (Sean, 2026-11-28)

icon-sdl was the oracle because its iconx drew as gcon's SDL server
did; since the SDL server stopped using Blend2D it no longer does, and
gcon needs only Arizona and itself. `vm/tests/iconx.sh` becomes
`arizona.sh`: the graphics programs gcon ships run under gcon (with the
X11 server) and under Arizona's own iconx, built with its X11 graphics
from `../icon` in a private copy (`tools/arizona/build.sh`, cached; its
IPL translated by its own build), both on a private Xvfb, and their
output and exit status must match. All seven do, colrpick too (Arizona
passes `display=` as gcon now does, so no special case). Pictures are
not compared there (Arizona's iconx gives no canvases up); `ref.sh`
compares drawing with Arizona's images. `ppmcmp.c` went with the pixel
comparison; the servers' `ICONSDL_TEST_DUMP` hook stays, for comparing
runs by hand.

### The antialias attribute (Sean, 2026-11-28)

`antialias=on|off` is a context attribute of gcon's, like `linestyle`
(copied by `Clone`): on by default (the Mac's quality first),
`GCONANTIALIAS=off` making off the default for every program. The GC
carries it (protocol 5: a byte after fillstyle). It smooths shapes and
text drawn in copy mode without a pattern: the raster servers (SDL,
headless; text too, the glyphs' coverage going through the mask) blend
by coverage, the Cocoa server sets Core Graphics's own switch on its
native drawing; the X11 server ignores it, X's core drawing being
aliased. Other drawops and patterns are always aliased, X's way, so
that rubber-banding and stipples keep their meaning. No hinting (Sean).

`ref.sh` sets `GCONANTIALIAS=off`, so it measures every server's
fidelity to X: SDL and headless 2, 3 and 13 pixels; Cocoa, aliased by
Core Graphics, 707, 458 and 393 (`ref/tolerance-cocoa`). The default is
seen in `graphics.sh` (which unsets it) and the window tests, which pass
as they did. A sample of Sean's fonts (Adobe's CFF Garamond, Caslon,
Arno and Chaparral; Consolas, DejaVu and the 13 MB Cyberbit in
TrueType) rendered well antialiased and kerned; a Type 1 font was
refused cleanly, as stb_truetype does not read Type 1.

### The Wayland server (2026-11-28)

`wayland/wplat.c` is the platform layer (about 1300 lines) over
libwayland-client, with `../raster` for canvases and fonts (fontconfig
through `../src/fontlinux.c`), as the SDL server has. Libraries (Sean):
libwayland-client, xkbcommon (keys to X keysyms), wayland-cursor (themed
pointers), and libdecor where found at build time; `build.sh` generates
xdg-shell and xdg-decoration from the system's wayland-protocols with
wayland-scanner.

- **Showing a canvas**: two shared-memory XRGB8888 buffers (the
  canvas's 0xFFRRGGBB as it is), each with the device rectangle it lacks
  (the canvas's changes since it was last shown, joined); a free one is
  brought up to date, the overlay (text cursor) painted on, and
  committed at the canvas's scale (`ICONSDL_SCALE`, else the largest
  output's). With both at the compositor, the present waits for a
  release. Nothing is attached before the first configure, which also
  sends `EXPOSE` (gcon-vm reports `&resize` only once exposed).
- **Events** are read whenever the reader polls, waits, opens or
  presents, so pings are answered and buffers return while a program
  draws without reading events. One thread, as the X11 server.
- **Keys**: xkbcommon's keysym and character, as XLookupString gives
  them (Latin-1 character, control and shift applied, else the keysym;
  modifiers alone nothing); repeated here, at the compositor's rate, as
  Wayland leaves repeating to clients. Scripted keys
  (`ICONSDL_TEST_EVENTS`) go through the compositor's keymap, else
  xkbcommon's default (a headless compositor may have no keyboard).
- **Decorations**: server-side through xdg-decoration where offered
  (KDE, sway); else libdecor's (GNOME draws none); `GCONLIBDECOR=off`
  for neither. libdecor's GTK plugin complains on stderr where the
  compositor has no seat (headless Weston), so `wayland.sh` points
  libdecor at its cairo plugin.
- **What Wayland withholds** is not done: placing a window, reading its
  position (`pl_position` fails, and so `WAttrib("pos")`: gcon-vm now
  fails the attribute where a server cannot say), raising, lowering,
  warping or reading the pointer outside the windows, the bell,
  the window's icon.
- **Default and fallback**: `./build.sh` on Linux builds Wayland's and
  X11's where their libraries are, Wayland's first; gcon-vm, finding
  the default without a display, tries `gcon-graphics-x11` (an X
  session, `ssh -X`) before the headless server.

Tested in a Debian 13 arm64 VM under Weston's headless backend
(`vm/tests/wayland.sh`): `graphics.sh` as the SDL server's off macOS
(`graphics-fontconfig.std`), `ref.sh` at the headless server's
tolerances (2, 3 and 13 pixels: the same rasterizer), the window tests
with libdecor and without; clean under ASan and UBSan. A screenshot
under Weston's pixman renderer showed libdecor's title bar and the
antialiased canvas. Not yet seen on a desktop compositor (GNOME, KDE)
or at a scale above 1.
