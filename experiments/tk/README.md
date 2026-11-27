# Tcl/Tk from Icon, through loadfunc

An experiment, kept to come back to once gcon-graphics is done: a
Tcl/Tk binding for Gcon, as a C library loaded by `loadfunc`
(gcon-vm only; under Gcon's Go VM `loadfunc` is error 121). Native
widgets from Icon, beside (not instead of) Icon's own graphics.

```icon
link tk

procedure main()
   tk_init()
   tk("ttk::button", ".b", "-text", "Quit", "-command", "icon quit")
   tk_eval("pack .b")
   while e := tk_event() do
      case e of { "quit" | "close": break }
end
```

- `icontk.c` (about 150 lines): `tk_init`, `tk_eval` and `tk_event`,
  written to Icon 9.5's `icall.h`, so the same library also works with
  iconx.
- `tk.icn`: stubs that load those on first use (from `$FPATH`, as the
  IPL's `cfunc.icn` does), and helpers: `tk(word, ...)` makes one Tcl
  command of its arguments, each quoted with `tclq`; `tk_args()` gives
  the last event's words as a list.
- `tktest.icn`: every part, with no one at the keyboard (a transparent
  window; Tk makes the events). Its output is the same on iconx and
  gcon-vm but for Gcon's UTF-8 text (iconx shows `"héllo"` as bytes).
- `demo.icn`: a window with an entry, buttons and a canvas you click,
  and an animation run by Icon between events.

Build and run (Tcl/Tk 9 from pkg-config; macOS: Homebrew `tcl-tk`, Aqua):

    ./build.sh
    FPATH=. ../../bin/gcon run demo.icn

## Design

Tcl's interface is strings, which `icall.h` passes well, so the binding
is three functions and everything else is Tcl script. The one
constraint is that `icall.h` gives C no way to call Icon, so a Tk
callback cannot run an Icon procedure. The binding defines a Tcl
command, `icon`, instead: a callback runs `icon quit` or `icon click %x
%y`, which queues its words as an event, and the program takes events
with `tk_event()`, handling Tk's event loop until one comes. That is
the model of Icon's own `Event()`, and of a Go program reading events
from a channel; with `../chan` a task could own the event loop and send
events to the rest.

`tk_event(msec)` waits at most msec milliseconds (0: only what is
pending) and fails if nothing came, which gives Icon its turn between
events (the demo's animation). It fails too once every window is
gone. Closing the main window is the event `close`. A Tcl error is
run-time error 500 with Tcl's message as the offending value.

## To do, when this is taken up

- Ship it: Gcon could build `libicontk.so` and put `tk.icn` on the link
  path, with a default `$FPATH` (as iconx's) so `link tk` just works.
- Faster calls: each `tk_eval` parses a script; a `tk_call(word, ...)`
  taking the words as separate arguments (Tcl_EvalObjv) would skip the
  quoting and parsing.
- A higher-level Icon layer (widgets as records, callbacks as
  procedures looked up by event name), or the event loop as a `chan`
  task.
- Linux (X11) is untested; Tk must run on the main thread on macOS,
  which gcon-vm does.
