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
