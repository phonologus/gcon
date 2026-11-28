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
