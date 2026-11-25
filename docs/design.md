# gcon-graphics design notes

## Role

gcon-graphics is gcon's display server. The decision and its reasons are
in gcon's `docs/design.md` ("Graphics: a separate display server"). In
brief: gconx owns Icon's graphics semantics (the event queue as an Icon
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
need no reply, and few messages that do. Rules carried from gcon's
design:

- drawing requests are pipelined; gconx flushes when it blocks, flushes
  explicitly, needs a reply, or exits;
- replies only where an answer is needed: `Pixel` (one per rectangle),
  opening windows and fonts, geometry queries;
- nothing that can fail in Icon terms is left to an asynchronous error;
  a failing pipelined request is a protocol error;
- font metrics and glyph advances come back when a font opens, so text
  width needs no round trip;
- image files are read and written by gconx; the server sends and
  receives pixels, and never opens the program's files;
- windows (canvases) and graphics contexts are separate objects, so
  `Clone` and `Couple` map onto them as onto X11's drawables and GCs;
- a version handshake comes first;
- events flow back unsolicited, in the X conventions `plat.c` already
  produces.

The transport is a socketpair inherited from gconx (the server is its
child), or a named socket.

## To do

1. Write the protocol specification (`docs/protocol.md`): message
   layout, ids, the handshake, the request and event set.
2. A headless Go server in gcon, drawing into `image.RGBA` or recording
   requests, for gcon's tests; a conformance suite over the protocol that
   both servers pass.
3. The protocol reader here, replacing `pl_main`'s interpreter thread.
4. Bring over icon-sdl's reference and window tests in a form that
   drives the server through the protocol.
