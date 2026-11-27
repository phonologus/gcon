# Keywords

The keywords, except the graphics keywords ([Graphics](graphics.md)).
Some are constants, some describe the running program, and some
(`&pos`, `&subject`, `&random`, `&trace`, `&error`, `&dump`) can be
assigned to.

## &allocated

```
&allocated
```

Generates four integers: the bytes allocated since the program started,
in all, in the static region, for strings, and for other blocks. Gcon has
one heap, so the static count is 0 and the total is the sum of the last
two.

```icon
procedure main()
   L := []
   every put(L, &allocated)
   write(*L, " ", L[1] = L[3] + L[4] & "total is strings and blocks", " ", L[2])
end
```

Output:

```
4 total is strings and blocks 0
```

**See also:** [&collections](#collections), [&regions](#regions), [&storage](#storage).

## &ascii

```
&ascii
```

The cset of the 128 ASCII characters.

```icon
procedure main()
   write(*&ascii, " ", *(&cset -- &ascii))
end
```

Output:

```
128 128
```

**See also:** [&cset](#cset).

## &clock

```
&clock
```

The time of day, as a string `"hh:mm:ss"` (24-hour).

```icon
procedure main()
   write(*&clock, " ", &clock ? (tab(3), =":", tab(6), =":", "hh:mm:ss"))
end
```

Output:

```
8 hh:mm:ss
```

**See also:** [&date](#date), [&dateline](#dateline).

## &collections

```
&collections
```

Generates four integers: the number of garbage collections since the
program started, in all, then (as Icon 9.5 counts them by region) of the
static, string and block regions; in Gcon, which has one heap, the last
three are 0.

```icon
procedure main()
   collect()
   L := []
   every put(L, &collections)
   write(*L, " ", L[1] > 0 & "collected")
end
```

Output:

```
4 collected
```

**See also:** [collect](system.md#collect), [&allocated](#allocated).

## &column

```
&column
```

Always fails: Icon 9.5 reserves it, and so does Gcon.

```icon
procedure main()
   write(&column | "no column")
end
```

Output:

```
no column
```

**See also:** [&line](#line).

## &cset

```
&cset
```

The cset of all 256 byte values.

```icon
procedure main()
   write(*&cset)
end
```

Output:

```
256
```

**See also:** [&ascii](#ascii), [&uset](#uset).

## &current

```
&current
```

The co-expression that is running; `&main` until another is activated.

```icon
procedure main()
   C := create write(image(&current === &main) | "not main")
   @C
   write(&current === &main & "main again")
end
```

Output:

```
not main
main again
```

**See also:** [&main](#main), [&source](#source).

## &date

```
&date
```

Today's date, as a string `"yyyy/mm/dd"`.

```icon
procedure main()
   write(*&date, " ", &date[5] == "/" & "yyyy/mm/dd")
end
```

Output:

```
10 yyyy/mm/dd
```

**See also:** [&clock](#clock), [&dateline](#dateline).

## &dateline

```
&dateline
```

The date and time in words, as `"Friday, November 27, 2026  5:56 pm"`.

```icon
procedure main()
   write(find(", ", &dateline) & "day, month day, year  time")
end
```

Output:

```
day, month day, year  time
```

**See also:** [&clock](#clock), [&date](#date).

## &digits

```
&digits
```

The cset of the ten digits, `'0123456789'`.

```icon
procedure main()
   write(many(&digits, "2026 AD"))
end
```

Output:

```
5
```

## &dump

```
&dump
```

If it is not 0 when the program ends (normally or by an error),
[display](system.md#display) writes the variables of every procedure
call and the globals to `&errout`. 0 at first.

```icon
procedure main()
   &dump := 1
   x := 3
end
```

This example is only translated: the dump goes to `&errout` at the end.

**See also:** [display](system.md#display).

## &e

```
&e
```

The base of the natural logarithms, 2.718281828...

```icon
procedure main()
   write(&e, " ", log(&e))
end
```

Output:

```
2.718281828 1.0
```

## &error

```
&error
```

Whether run-time errors are converted to failure: while it is not 0, an
error that can be makes the expression fail instead of ending the
program, and records it in `&errornumber`, `&errortext` and
`&errorvalue`. A positive value counts down by one with each conversion;
a negative one converts every error. 0 at first.

```icon
procedure main()
   &error := 1
   ([] + 1) | write("converted: ", &errornumber, ", ", &error, " left")
end
```

Output:

```
converted: 102, 0 left
```

**See also:** [&errornumber](#errornumber), [errorclear](system.md#errorclear).

## &errornumber

```
&errornumber
```

The number of the last run-time error converted to failure; fails if
there has been none (or after [errorclear](system.md#errorclear)).

```icon
procedure main()
   write(&errornumber | "no error yet")
   &error := -1
   integer("x") + 1
   "x" + 1
   write(&errornumber)
end
```

Output:

```
no error yet
102
```

**See also:** [&error](#error), [&errortext](#errortext), [&errorvalue](#errorvalue).

## &errortext

```
&errortext
```

The message of the last run-time error converted to failure; fails if
there has been none.

```icon
procedure main()
   &error := 1
   [] + 1
   write(&errortext)
end
```

Output:

```
numeric expected
```

**See also:** [&errornumber](#errornumber).

## &errorvalue

```
&errorvalue
```

The offending value of the last run-time error converted to failure;
fails if there has been none, or if the error had no value.

```icon
procedure main()
   &error := 1
   "abc" + 1
   write(image(&errorvalue))
end
```

Output:

```
"abc"
```

**See also:** [&errornumber](#errornumber).

## &errout

```
&errout
```

The standard error file.

```icon
procedure main()
   write(type(&errout), " ", image(&errout))
end
```

Output:

```
file &errout
```

**See also:** [&input](#input), [&output](#output).

## &eventcode

```
&eventcode
```

`&null`: with `&eventsource` and `&eventvalue`, a keyword of Icon's
event monitoring, which neither Icon 9.5's iconx nor Gcon runs.

```icon
procedure main()
   write(image(&eventcode))
end
```

Output:

```
&null
```

## &eventsource

```
&eventsource
```

`&null`, as [&eventcode](#eventcode).

```icon
procedure main()
   write(image(&eventsource))
end
```

Output:

```
&null
```

## &eventvalue

```
&eventvalue
```

`&null`, as [&eventcode](#eventcode).

```icon
procedure main()
   write(image(&eventvalue))
end
```

Output:

```
&null
```

## &fail

```
&fail
```

Fails: an expression that produces nothing.

```icon
procedure main()
   write(&fail | "failed")
end
```

Output:

```
failed
```

## &features

```
&features
```

Generates the names of the features this Gcon has, with iconx's names:
`"UNIX"`, `"ASCII"`, `"co-expressions"`, `"dynamic loading"`,
`"environment variables"`, `"external values"`, `"keyboard functions"`,
`"large integers"`, `"pipes"`, `"system function"`, `"graphics"` and
`"SDL graphics"` (in place of `"X Windows"`), and `"Macintosh"` on
macOS. Each has a predefined preprocessor symbol (`_UNIX`, `_GRAPHICS`,
`_SDL_GRAPHICS`, ...).

```icon
procedure main()
   write(&features == "large integers")
   write(&features == "X Windows" | "no X")
end
```

Output:

```
large integers
no X
```

## &file

```
&file
```

The name of the source file of the code that is running.

```icon
procedure main()
   write(&file[-4:0])
end
```

Output:

```
.icn
```

**See also:** [&line](#line).

## &host

```
&host
```

The name of the computer the program runs on.

```icon
procedure main()
   write(type(&host), " ", *&host > 0 & "named")
end
```

Output:

```
string named
```

## &input

```
&input
```

The standard input file, which [read](files.md#read) and
[reads](files.md#reads) use by default.

```icon
procedure main()
   write(read() | "no input")
end
```

Output:

```
no input
```

**See also:** [&output](#output).

## &lcase

```
&lcase
```

The cset of the 26 lower-case letters.

```icon
procedure main()
   write(image(&lcase ** 'Hello'))
end
```

Output:

```
'elo'
```

**See also:** [&ucase](#ucase), [&letters](#letters).

## &letters

```
&letters
```

The cset of the 52 letters, upper and lower case.

```icon
procedure main()
   write(*&letters)
end
```

Output:

```
52
```

## &level

```
&level
```

The depth of procedure calls: 1 in `main`, 2 in a procedure `main`
calls, and so on.

```icon
procedure f()
   return &level
end
procedure main()
   write(&level, " ", f())
end
```

Output:

```
1 2
```

## &line

```
&line
```

The number of the source line of the code that is running.

```icon
procedure main()
   write(&line)
end
```

Output:

```
2
```

**See also:** [&file](#file).

## &main

```
&main
```

The co-expression of the main program, which starts the program and
runs `main`.

```icon
procedure main()
   write(image(&main))
end
```

Output:

```
co-expression_1(1)
```

**See also:** [&current](#current).

## &null

```
&null
```

The null value, the value of a variable never assigned.

```icon
procedure main()
   local x
   write(x === &null & "null")
end
```

Output:

```
null
```

## &output

```
&output
```

The standard output file, which [write](files.md#write) uses by
default.

```icon
procedure main()
   write(&output, "to standard output")
end
```

Output:

```
to standard output
```

**See also:** [&errout](#errout), [&input](#input).

## &phi

```
&phi
```

The golden ratio, 1.618033989...

```icon
procedure main()
   write(&phi, " ", &phi ^ 2 - &phi)
end
```

Output:

```
1.618033989 1.0
```

## &pi

```
&pi
```

The ratio of a circle's circumference to its diameter, 3.141592654...

```icon
procedure main()
   write(&pi, " ", cos(&pi))
end
```

Output:

```
3.141592654 -1.0
```

## &pos

```
&pos
```

The position of string scanning in `&subject`, an integer from 1 to
`*&subject + 1`. Assigning a position to it moves there (0 and negative
positions count from the end, and it holds the position from the start);
assigning one outside the subject fails. Scanning (`s ? e`) sets it to
1, and assigning to `&subject` does too.

**Errors:** 101 if the value assigned is not an integer.

```icon
procedure main()
   &subject := "hello"
   &pos := 0
   write(&pos)
   write((&pos := 9) | "outside")
end
```

Output:

```
6
outside
```

**See also:** [&subject](#subject).

## &progname

```
&progname
```

The name of the program: as it was run for a built program, or for
`gcon run prog.icn`, what the built program would be called
(`./prog`). It can be assigned to.

```icon
procedure main()
   &progname := "renamed"
   write(&progname)
end
```

Output:

```
renamed
```

## &random

```
&random
```

The seed of the random numbers that `?` produces: each use moves it on.
Assigning to it starts a repeatable sequence; the same seed gives
iconx's sequence. 0 at first.

```icon
procedure main()
   &random := 0
   write(?100, " ", ?100)
   &random := 0
   write(?100)
end
```

Output:

```
22 42
22
```

## &regions

```
&regions
```

Generates three integers, the sizes of iconx's static, string and block
regions. Gcon has one heap that grows as it needs to, so these are
iconx's initial sizes: 0, 500000 and 500000.

```icon
procedure main()
   every writes(&regions, " ")
   write()
end
```

Output:

```
0 500000 500000 
```

**See also:** [&storage](#storage).

## &source

```
&source
```

The co-expression that activated the one running: where `@` gives a
value back to (`v @ &source`).

```icon
procedure main()
   C := create {
      write(image(&source === &main) | "no")
      "result" @ &source
   }
   write(@C)
end
```

Output:

```
co-expression_1(1)
result
```

**See also:** [&current](#current), [&main](#main).

## &storage

```
&storage
```

Generates three integers, the space used in iconx's static, string and
block regions; Gcon has no such regions, and gives 0, 0 and 0.
[&allocated](#allocated) counts what is allocated.

```icon
procedure main()
   every writes(&storage, " ")
   write()
end
```

Output:

```
0 0 0 
```

**See also:** [&regions](#regions).

## &subject

```
&subject
```

The string that string scanning examines. `s ? e` sets it to `s` (and
`&pos` to 1) for the scanning expression `e`, and restores both after.
Assigning to it sets `&pos` to 1.

**Errors:** 103 if the value assigned is not a string.

```icon
procedure main()
   "outer" ? {
      "inner" ? write(&subject)
      write(&subject)
   }
end
```

Output:

```
inner
outer
```

**See also:** [&pos](#pos).

## &time

```
&time
```

The processor time the program has used, in milliseconds.

```icon
procedure main()
   t := &time
   every 1 to 100000
   write(&time >= t & "time passes")
end
```

Output:

```
time passes
```

## &trace

```
&trace
```

Whether procedure calls are traced: while it is not 0, each call,
return, suspension, resumption and failure (and co-expression
activation) writes a line to `&errout`, and a positive value counts
down by one with each line. Its value at first is the environment
variable `TRACE`'s, or 0.

```icon
procedure main()
   &trace := -1
   write("traced")
end
```

This example is only translated: its trace names the file it runs from.

## &ucase

```
&ucase
```

The cset of the 26 upper-case letters.

```icon
procedure main()
   write(map("Hello", &ucase, &lcase))
end
```

Output:

```
hello
```

**See also:** [&lcase](#lcase).

## &uset

```
&uset
```

The uset of every character: the ASCII characters, the 128 bytes from
0x80, and every Unicode code point but the surrogates. A Gcon addition,
not in Icon 9.5. `tab(any(&uset))` is the next whole character of the
subject.

```icon
procedure main()
   "é!" ? {
      write(tab(any(&uset)))
      write(&pos)
   }
end
```

Output:

```
é
3
```

**See also:** [uset](unicode.md#uset).

## &version

```
&version
```

The version of Icon this is: `"Icon Version 9.5 (Gcon)"`.

```icon
procedure main()
   write(&version)
end
```

Output:

```
Icon Version 9.5 (Gcon)
```
