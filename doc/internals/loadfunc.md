# C functions

`loadfunc(file, name)` loads a C function from a shared library and
returns it as an Icon function, as in Icon 9.5. Functions are written
to Icon 9.5's `icall.h`, which gcon-vm ships in `vm/include` unchanged,
so existing code recompiles as it is. The IPL's C functions
(`lib/ipl/cfuncs`, 29 functions) are built by `build.sh` into the
root's `lib/libcfunc.so`, and `link cfunc` declares them.

## Loading

- The library's handle is kept for the next call with the same file
  name; `dlsym` is tried again with an underscore before the name.
- A missing library or function writes iconx's diagnostic
  (`loadfunc("f","n"): ` and the loader's message) to `&errout` and
  raises run-time error 216.
- The result is a function: `image` gives `function name`, and `args`
  gives -1, as for iconx's built-ins that take any number of arguments.
- The IPL's `pathload` (which `link cfunc` uses) finds the library
  through `FPATH`, to which gcon-vm appends the root's `lib`, as iconx
  appends its own directory.

## Calls

A C function sees iconx's descriptors, so each call converts the
arguments to descriptors in iconx's layout, with `argv[0]` set to
`&null`, and treats the status as iconx does: 0 returns `argv[0]`, -1
fails, and n raises run-time error n, with `argv[0]` as the offending
value unless it is `&null`. A result gcon-vm cannot represent is error
123.

- **Memory:** everything made for a call (string copies, blocks, and
  what C allocates with `alcstr`, `alcreal` and `alcfile`) is in an
  arena freed when the call returns, after the result is converted and
  copied into gcon-vm's heap. C code never holds a pointer into the
  heap, so the collector cannot disturb it, and a string argument is a
  NUL-terminated copy. A pointer kept from one call to the next is
  invalid, as it is in iconx after a collection.
- **Large integers** are passed as iconx's blocks (half-word digits,
  most significant first) and converted back.
- **Values C cannot take apart** (csets, records, lists, sets, tables,
  procedures, co-expressions) are passed as blocks with iconx's type
  codes, so `IconType` is right, and convert back to the values they
  stand for. A list's block begins as iconx's, so `ListLen` gives its
  size and `cpslots` its elements.
- **Files** are lent as stdio streams on a duplicate of the file
  descriptor, closed after the call; gcon-vm's buffered output is
  written first. A file C makes (`RetFile`) becomes a gcon-vm file.
- **External values** (`alcexternal`) are blocks in gcon-vm's heap, in
  iconx's layout; `type`, `image`, `copy` and sorting call the block's
  functions, with iconx's defaults.

## Building a library

A library leaves the callbacks `icall.h` declares unresolved, for the
executable to supply; gcon-vm exports them. As `ipl/cfuncs/mklib.sh`
builds them:

```sh
cc -O2 -fPIC -I GCONROOT/vm/include -c f.c
cc -bundle -undefined dynamic_lookup -o f.so f.o    # macOS
cc -shared -fPIC -o f.so f.o                        # elsewhere
```

`peek` and `spy` (`internal.c`) read memory at iconx's addresses and
mean nothing under gcon-vm.
