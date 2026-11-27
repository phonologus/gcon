# The virtual machine

gcon-vm (`vm/src`) runs built programs. It is C11, libc only, about the
size of iconx's run-time system, and gives iconx's results: its
built-in functions follow iconx's C (`fstr.r`, `rlrgint.r`, ...) where
the language defines what they do.

## Values

A value is 16 bytes: a header word, holding a kind (one byte), flags and
a 48-bit field, and a data word, which is an integer, a real or a
pointer. Strings are a pointer and a length (in the header's field), so
taking a substring allocates nothing; csets are 256 bits; large
integers are sign and magnitude in 32-bit digits. The kinds are
Icon's types (null, integer, real, string, cset, list, table, set,
record, procedure, co-expression, file, ...) and the VM's own: the
variables (a local, a list element, a field, a substring, a keyword),
frames, generators and scanning environments, which programs never see.

Strings are bytes, as in Icon 9.5. A uset ([Strings and
Unicode](../unicode.md)) is a cset's 256 bits for the ASCII characters
and the bytes from 0x80, and sorted ranges for the code points beyond.

## Frames and generators

A procedure call makes a **frame**: the procedure, its locals and
temporaries, the instruction it is at, and its caller. Frames are
allocated in the heap, not on the C stack, so the interpreter's loop
never recurses: call, return, suspend, resume and failure move from
frame to frame ([the instructions](instructions.md)). A suspended
frame stays alive in its caller's handle until it is resumed or
dropped; a frame that has returned is recycled at once if nothing can
refer to it.

A **built-in generator** (`to`, `!`, `upto`, `find`, ...) is a value
with a `next` function that produces the next result or fails: a
state machine, not a function that calls back. No Icon 9.5 built-in
calls back into Icon code, so nothing re-enters the interpreter.

The depth of Icon calls is limited by `MSTKSIZE` (iconx's variable, in
words; run-time error 301 beyond it), not by the C stack, so deep
recursion fails cleanly.

## Co-expressions

A co-expression is a chain of frames, with its activator, its state and
the copy of the locals that `^C` (refresh) starts again from. `@`
switches the VM's current chain, so an activation costs about as much
as a procedure call, where iconx gives each co-expression its own C
stack and switches in assembly (about 2,500 ns; Gcon, about 12 ns).
Transmission (`v @ C`), symmetric transfer, `&source`, `&current` and
`&main` follow from that. Unreachable co-expressions are collected like
any other value, and there is no limit on their number.

## The collector

Memory is one heap, collected precisely by mark and sweep. A collection
runs only at the interpreter's safe point, between instructions, when
no C code holds a pointer the roots do not: so built-in functions
allocate freely, and the roots are exactly the VM's (globals,
constants, frames and co-expressions, and a few fields of the VM).

The heap is 1 MB chunks, each of blocks of one size class (16 bytes to
32 KB), and larger objects in chunks of their own. Every block has a
type, so a pointer into any part of an object (a substring, a local of
a frame) finds the object and how to trace it. A collection runs when
the bytes allocated since the last exceed the live data (at least 16
MB).

`&collections` counts collections and `&allocated` the bytes
allocated; iconx's regions do not exist, so `&regions` gives iconx's
initial sizes and `&storage` zeros, and `BLKSIZE`, `STRSIZE` and the
other memory variables have no effect.

## Errors and tracing

A run-time error gives iconx's message, offending value and traceback,
and `&error` converts errors to failure as in iconx. `&trace` (set from
`TRACE`) traces calls, returns, suspensions, resumptions and failures,
and co-expression activations, in iconx's form.

## Graphics

gcon-vm owns Icon's graphics semantics (`vm/src/graphics.c`): windows
and their contexts, attributes, events, and the attribute strings of
`WAttrib`. Drawing itself is the display server's: gcon-vm starts it
when a program first opens a window and sends it
[the protocol](protocol.md)'s requests, buffered until it needs a
reply.
