# The program and its system

Ending the program, run-time errors, the environment, commands, time,
the keyboard and the collector.

## collect

```
collect(i1, i2)
```

Runs the garbage collector and produces `&null`. Gcon has one heap, so
the region `i1` and the amount `i2` that Icon 9.5 asks for make no
difference, beyond their checks.

**Defaults:** `i1` 0; `i2` 0.

**Errors:** 101 if `i1` or `i2` is not an integer; 205 if either is
out of range.

```icon
procedure main()
   collect()
   write(&collections > 0 & "collected")
end
```

Output:

```
collected
```

**See also:** [&collections](keywords.md#collections).

## delay

```
delay(i)
```

Waits `i` milliseconds, then produces `&null`.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   t := &time
   delay(50)
   write(&time - t >= 0 & "waited")
end
```

Output:

```
waited
```

## display

```
display(i, f)
```

Writes to the file `f` the local variables of the `i` most recent
procedure calls, newest first, then the global variables, with their
values; produces `&null`.

**Defaults:** `i` `&level` (every call); `f` `&errout`.

**Errors:** 101 if `i` is not an integer; 105 if `f` is not a file; 205
if `i` is negative; 213 if `f` is not open for writing.

```icon
global count
procedure tally(n)
   local seen
   seen := "yes"
   display(1, &output)
end
procedure main()
   count := 3
   tally(5)
end
```

Output:

```
co-expression_1(1)

tally local identifiers:
   n = 5
   seen = "yes"

global identifiers:
   count = 3
   display = function display
   main = procedure main
   tally = procedure tally
```

**See also:** [&level](keywords.md#level).

## errorclear

```
errorclear()
```

Clears the record of the last error converted to failure, so that
`&errornumber`, `&errortext` and `&errorvalue` fail again; produces
`&null`.

```icon
procedure main()
   &error := 1
   [] + 1 | write(&errornumber)
   errorclear()
   write(&errornumber | "cleared")
end
```

Output:

```
102
cleared
```

**See also:** [&error](keywords.md#error), [&errornumber](keywords.md#errornumber).

## exit

```
exit(i)
```

Ends the program with the exit status `i`, after writing out what is
buffered for its files.

**Defaults:** `i` 0.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   write("done")
   exit(0)
   write("never")
end
```

Output:

```
done
```

**See also:** [stop](#stop).

## getch

```
getch()
```

Reads one byte from the keyboard (standard input), without echoing it
and without waiting for a newline when standard input is a terminal,
and produces it as a one-byte string; fails at the end of the input.

```icon
procedure main()
   write(getch() | "no input")
end
```

Output:

```
no input
```

**See also:** [getche](#getche), [kbhit](#kbhit).

## getche

```
getche()
```

Reads one byte from the keyboard as [getch](#getch) does, but echoes it.

```icon
procedure main()
   write(getche() | "no input")
end
```

Output:

```
no input
```

**See also:** [getch](#getch).

## getenv

```
getenv(s)
```

Produces the value of the environment variable `s`; fails if it is not
set.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   write(getenv("NO_SUCH_VARIABLE_FOR_GCON") | "not set")
end
```

Output:

```
not set
```

## kbhit

```
kbhit()
```

Produces `&null` if a byte is waiting to be read from the keyboard
(standard input); fails otherwise.

```icon
procedure main()
   if kbhit() then
      write("a key is waiting: ", getch())
   else
      write("no key yet")
end
```

Whether a key is waiting depends on how the program is run (at the end of
redirected input, `kbhit` succeeds), so this example is not run.

**See also:** [getch](#getch).

## loadfunc

```
loadfunc(s1, s2)
```

Loads the C function `s2` from the shared library `s1` and produces it
as a function. [C functions](../internals/loadfunc.md) describes the
interface; `link cfunc` declares the IPL's C functions.

**Errors:** 103 if `s1` or `s2` is not a string; 216 if the library or
the function cannot be found (with the loader's message on `&errout`).

```icon
procedure main()
   f := loadfunc("/usr/local/lib/mylib.so", "myfunc")
   write(f("argument"))
end
```

This example is only translated: the library is yours to supply.

## runerr

```
runerr(i, x)
```

Raises run-time error `i`, with `x` as the offending value if it is
given. An error number of Icon's has its usual message; any other has
none.

**Errors:** 101 if `i` is not an integer; 205 if it is not positive.

```icon
procedure main()
   &error := 1
   runerr(500, "value") | write(&errornumber, " ", image(&errorvalue), " ", &errortext)
end
```

Output:

```
500 "value" program malfunction
```

**See also:** [&error](keywords.md#error).

## stop

```
stop(x1, x2, ...)
```

Writes its arguments as [write](files.md#write) does, but to `&errout`
unless a file argument says otherwise, and ends the program with exit
status 1.

**Errors:** 109 if an argument is neither a string nor a file; 213 if a
file is not open for writing.

```icon
procedure main()
   stop(&output, "giving up: ", 42)
end
```

Output:

```
giving up: 42
```

**See also:** [exit](#exit).

## system

```
system(s)
```

Runs the command `s` with `sh`, waits for it, and produces its status as
the C library's `system` reports it: the exit code times 256, so 0 for
success.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   write(system("exit 3"), " ", system("true"))
end
```

Output:

```
768 0
```

**See also:** [open](files.md#open) (mode `p`).
