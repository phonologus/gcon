# The gcon graphics protocol

Between a client (gcon-vm, which owns Icon's graphics semantics) and a
display server (gcon-graphics, which owns windows, canvases and drawing).
Modelled on Plan 9's draw(3): binary messages, objects named by ids the
client chooses, drawing requests without replies. The rules behind it
are in gcon's `docs/design.md` ("Graphics: a separate display server")
and this repository's `docs/design.md`.

Version 1. Everything here is implemented unless marked *(later)*.

## Transport and framing

A stream socket: by default a socketpair, the client starting the
server as its child with the socket as a file descriptor named on the
command line (`gcon-graphics -fd N`).

Every message, either way, is

    u32 length      bytes that follow (opcode and body)
    u8  opcode
    ... body

Integers are little-endian: `u8`, `u32`, `i32`, `u64`, `i64`; `f64` is
an IEEE double. A string `str` is `u32 n` and n bytes (UTF-8 or bytes;
no terminator). A reply has the opcode of its request.

Requests are handled in order. Only those marked **reply** have one,
sent before the next request is read, so the client can pipeline every
other request and must read exactly one reply after each request that
has one. The client buffers requests and sends them when it needs a
reply, when it waits (`WAIT`), when it flushes, and when it exits.

A request that cannot be carried out (an unknown id, a malformed
message) is a protocol error: the server writes a message to its
standard error and exits. Everything that can fail in Icon terms is
either decided by the client or is a request with a reply.

## Objects

The client names objects with u32 ids, never 0, unique per kind while in
use:

- **canvas**: a picture in the server's memory (an RGBA image at an
  integer scale) and, unless hidden, the window that shows it. Icon's
  window state.
- **gc**: a graphics context, the drawing state a drawing request uses
  (colours, drawop, line and fill style, pattern, clipping). Icon's
  context holds more (font, dx, dy, leading, gamma); the client keeps
  that and sends the gc whenever the parts the server uses change.
- **font**: an opened font.

## Requests

### Connection

`HELLO` (0) **reply**: `u32 version`. Reply: `u32 version` (the
server's), `u32 ok` (1 if it speaks the client's version). First on
every connection.

### Canvases and windows

`CANVAS` (1) **reply**: `u32 id, i32 x, i32 y, i32 w, i32 h, u32 bg,
i32 state, u32 resizable, str title`. Makes a canvas of w × h logical
pixels filled with bg (0xRRGGBB) and, unless state is hidden, a window
showing it. x, y: the window's position, or `NOPOS` (-2147483648) for
the system's choice. state: 0 normal, 1 iconic, 2 maximal, 3 hidden.
Reply: `i32 ok, i32 scale` (the canvas's pixels per logical pixel).

`FREE` (2): `u32 id`. Closes the canvas's window and frees the canvas.

`RESIZE` (3): `u32 id, i32 w, i32 h, u32 bg`. Gives the canvas a new
size, keeping what fits and filling the rest with bg.

*(later)* window requests: title, geometry, position, state, raise,
lower, cursor, warp, beep, icon, the text-cursor overlay, display size,
pointer position.

### Graphics contexts

`GC` (4): `u32 id, u32 fg, u32 bg, u8 drawop, u8 reverse, i32
linewidth, u8 linestyle, u8 fillstyle, i32 clipx, i32 clipy, i32 clipw,
i32 cliph, i32 patw, i32 path, u8 pattern[patw * path]`. Defines or
redefines gc id. drawop is X's GC function (0 clear ... 15 set; 3
copy); reverse: drawop=reverse (xor with fg ^ bg). linestyle 0 solid, 1
dashed, 2 striped; fillstyle 0 solid, 2 masked, 3 textured. clipw < 0:
no clipping. Pattern cells nonzero where set; patw 0: none.

### Drawing

Each names a canvas and a gc; coordinates are logical pixels, origin top
left. None has a reply.

`POINTS` (5), `LINES` (6) (a polyline), `FILLPOLY` (7): `u32 canvas,
u32 gc, u32 n, i32 xy[2n]`.

`SEGMENTS` (8): `u32 canvas, u32 gc, u32 n, i32 xyxy[4n]`.

`RECTS` (9), `FILLRECTS` (10): `u32 canvas, u32 gc, u32 n, i32
xywh[4n]`.

`ARCS` (11): `u32 canvas, u32 gc, u8 fill, u32 n`, then n times `i32 x,
y, w, h, f64 start, f64 extent` (radians, as Icon's).

`ERASE` (12): `u32 canvas, u32 gc, i32 x, y, w, h`: fill with the gc's
bg.

`COPY` (13): `u32 src, u32 dst, u32 gc, i32 x, y, w, h, x2, y2`.

`WRITE` (14): `u32 canvas, u32 gc, i32 x, y, w, h, u32 px[w*h], u8
opaque[w*h]`: pixels 0xRRGGBB, drawn where opaque is nonzero.

*(later)* `TEXT`.

### Reading

`READ` (15) **reply**: `u32 canvas, i32 x, y, w, h, u32 bg`. Reply:
`u32 px[w*h]`, 0xRRGGBB; pixels outside the canvas read as bg.

### Colours

`COLOR` (16) **reply**: `str name`. An X colour name. Reply: `i32 ok,
i32 r, i32 g, i32 b` (8 bits each). Icon's own colour grammar is
the client's; the server knows only X's names.

### Showing and events

`WAIT` (17) **reply**: `i32 timeout` (milliseconds; -1 for ever).
Shows what has changed on every canvas's window, then waits until an
event arrives or the time-out passes (0: does not wait), and replies
with the events that have arrived: `u32 n`, then n times `u32 canvas,
i32 kind, i64 code, i32 x, i32 y, i64 mods, u64 ms`. Kinds, as the
platform layer gives them (X conventions): 1 a key giving a character
(code), 2 a key giving an X keysym (code), 3 button press (code 1, 2,
3), 4 button release, 5 motion (code: button mask), 6 resize (x, y the
new size), 7 move (x, y the new position), 8 expose, 9 state (code 0
normal, 1 iconic, 2 maximal), 10 close (the user closed the window).
mods: X modifier masks. ms: the time in milliseconds.

The client sends `WAIT 0` periodically while a program draws without
pausing, so windows are shown and events collected.

`SHOW` (18): `u8 full`. Shows what has changed (everything if full)
without collecting events: `WFlush`, `WSync`.

### Fonts and text *(later)*

`FONT`, `TEXT`, `WIDTH`: opening a font replies with its metrics and the
advances of its first 256 characters, so most text widths need no round
trip.

### Image files *(later)*

The server never opens the program's files: `DECODE` takes a file's
bytes and replies with pixels; `ENCODE` takes pixels and a format and
replies with the bytes.
