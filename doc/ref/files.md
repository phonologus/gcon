# Files

Opening, reading and writing files and pipes, and the file system.
`&input`, `&output` and `&errout` are the standard streams. Positions in
a file count bytes from 1, as positions in a string do. (`fattrib`,
which Icon 9.5 provides only on some systems, is not provided.)

## chdir

```
chdir(s)
```

Changes the current directory to `s` and produces `&null`; fails if it
cannot.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   write(chdir("/") & "changed")
   write(chdir("/no/such/directory") | "cannot")
end
```

Output:

```
changed
cannot
```

## close

```
close(f)
```

Closes the file `f` and produces it; for a pipe, waits for its command
to finish and produces the command's exit status.

**Errors:** 105 if `f` is not a file.

```icon
procedure main()
   p := open("echo hello; exit 3", "p")
   write(read(p))
   write(close(p))
end
```

Output:

```
hello
3
```

**See also:** [open](#open).

## flush

```
flush(f)
```

Writes out anything buffered for the file `f`, and produces `f`.

**Errors:** 105 if `f` is not a file.

```icon
procedure main()
   writes("working...")
   flush(&output)
   write(" done")
end
```

Output:

```
working... done
```

## open

```
open(s1, s2)
```

Opens the file named `s1` in the modes the letters of `s2` give, and
produces it; fails if it cannot. The modes, which combine:

| Letter | Mode |
|---|---|
| `r` | read |
| `w` | write, the file emptied or created |
| `a` | write, appending at the end |
| `b` | both read and write |
| `c` | create the file, emptied |
| `u` | untranslated (makes no difference on Unix) |
| `p` | a pipe: `s1` is a command for `sh`, read from or (with `w`) written to |

With `p`, `open` starts the command, whose output (or input) is the file.
`g` opens a window ([Graphics](graphics.md#opening-a-window)).

**Defaults:** `s2` `"r"`.

**Errors:** 103 if `s1` or `s2` is not a string; 209 if `s2` holds a
letter that is not a mode.

```icon
procedure main()
   f := open("/tmp/gcon-doc-open.txt", "w") | stop("cannot write")
   write(f, "first")
   close(f)
   f := open("/tmp/gcon-doc-open.txt")
   write(read(f))
   close(f)
   remove("/tmp/gcon-doc-open.txt")
   write(open("/no/such/file") | "cannot open")
end
```

Output:

```
first
cannot open
```

**See also:** [close](#close), [read](#read), [write](#write).

## read

```
read(f)
```

Produces the next line of the file `f`, without its newline (a line
ending in return and newline loses both); fails at the end of the file.
The last line need not end in a newline.

**Defaults:** `f` `&input`.

**Errors:** 105 if `f` is not a file; 212 if it is not open for reading.

```icon
procedure main()
   p := open("printf 'one\\ntwo\\nthree'", "p")
   while line := read(p) do write("[", line, "]")
   close(p)
end
```

Output:

```
[one]
[two]
[three]
```

**See also:** [reads](#reads), [write](#write).

## reads

```
reads(f, i)
```

Produces the next `i` bytes of the file `f`, or what is left if that is
fewer; fails at the end of the file. Newlines are bytes like any other.

**Defaults:** `f` `&input`; `i` 1.

**Errors:** 101 if `i` is not an integer; 105 if `f` is not a file; 205
if `i` is not positive; 212 if `f` is not open for reading.

```icon
procedure main()
   p := open("printf 'one\\ntwo'", "p")
   write(image(reads(p)), " ", image(reads(p, 4)), " ", image(reads(p, 10)))
   write(reads(p) | "at the end")
   close(p)
end
```

Output:

```
"o" "ne\nt" "wo"
at the end
```

**See also:** [read](#read), [seek](#seek).

## remove

```
remove(s)
```

Removes the file named `s` and produces `&null`; fails if it cannot.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   close(open("/tmp/gcon-doc-remove.txt", "w"))
   write(remove("/tmp/gcon-doc-remove.txt") & "removed")
   write(remove("/tmp/gcon-doc-remove.txt") | "already gone")
end
```

Output:

```
removed
already gone
```

**See also:** [rename](#rename).

## rename

```
rename(s1, s2)
```

Renames the file `s1` to `s2` and produces `&null`; fails if it cannot.

**Errors:** 103 if `s1` or `s2` is not a string.

```icon
procedure main()
   close(open("/tmp/gcon-doc-old.txt", "w"))
   write(rename("/tmp/gcon-doc-old.txt", "/tmp/gcon-doc-new.txt") & "renamed")
   write(rename("/tmp/gcon-doc-old.txt", "/tmp/x") | "no such file")
   remove("/tmp/gcon-doc-new.txt")
end
```

Output:

```
renamed
no such file
```

**See also:** [remove](#remove).

## seek

```
seek(f, i)
```

Moves to position `i` of the file `f` (1 is the first byte; 0 is the
end; a negative position counts back from the end) and produces `f`;
fails if it cannot, as on a pipe.

**Errors:** 101 if `i` is not an integer; 105 if `f` is not a file.

```icon
procedure main()
   f := open("/tmp/gcon-doc-seek.txt", "w")
   writes(f, "abcdef")
   close(f)
   f := open("/tmp/gcon-doc-seek.txt")
   seek(f, 3)
   write(reads(f, 2), " ", where(f))
   seek(f, -2)
   write(reads(f, 9))
   close(f)
   remove("/tmp/gcon-doc-seek.txt")
end
```

Output:

```
cd 5
ef
```

**See also:** [where](#where).

## where

```
where(f)
```

Produces the current position in the file `f`, counting from 1; fails if
it has none, as a pipe.

**Errors:** 105 if `f` is not a file.

```icon
procedure main()
   f := open("/tmp/gcon-doc-where.txt", "w")
   write(where(f))
   writes(f, "abc")
   write(where(f))
   close(f)
   remove("/tmp/gcon-doc-where.txt")
end
```

Output:

```
1
4
```

**See also:** [seek](#seek).

## write

```
write(x1, x2, ...)
```

Writes its arguments, each converted to a string, then a newline, and
produces the last argument (`&null` if there is none, which writes an
empty line). A file argument is not written: it changes where the
arguments after it go (at first `&output`), and the newline goes to the
last file named.

**Errors:** 109 if an argument is neither a string nor a file (nor can
be converted); 213 if a file is not open for writing.

```icon
procedure main()
   write("total: ", 3 + 4)
   x := write("a", "b")
   write(x)
end
```

Output:

```
total: 7
ab
b
```

**See also:** [writes](#writes), [read](#read).

## writes

```
writes(x1, x2, ...)
```

Writes its arguments as [write](#write) does, but without the newline.

**Errors:** 109 if an argument is neither a string nor a file (nor can
be converted); 213 if a file is not open for writing.

```icon
procedure main()
   every writes(1 to 5, " ")
   writes("\n")
end
```

Output:

```
1 2 3 4 5 
```

**See also:** [write](#write).
