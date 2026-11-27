# Values and types

Converting values from one type to another, finding out what a value
is, and the procedures and variables of a running program.

## args

```
args(x)
```

Produces the number of parameters of the procedure, function or record
constructor `x`. A procedure whose last parameter collects the rest
(`p(a, b[])`) gives minus its number of parameters; a built-in function
that takes any number of arguments gives -1.

**Errors:** 106 if `x` is not a procedure, function or record
constructor.

```icon
record point(x, y)
procedure p(a, b)
end
procedure q(a, b[])
end
procedure main()
   write(args(p), " ", args(q), " ", args(left), " ", args(write), " ", args(point))
end
```

Output:

```
2 -2 3 -1 2
```

**See also:** [proc](#proc).

## copy

```
copy(x)
```

Produces a copy of `x` if it is a structure (a list, table, set or
record), and `x` itself otherwise. The copy is of one level: its
elements are the same values as the original's, so a structure inside
it is shared, not copied. A copied table keeps the original's default.

```icon
procedure main()
   L := [1, [2]]
   M := copy(L)
   M[1] := 9
   M[2][1] := 8
   write(L[1], " ", L[2][1])
end
```

Output:

```
1 8
```

**See also:** [serial](#serial).

## cset

```
cset(x)
```

Produces `x` converted to a cset: the set of the bytes of `x` as a
string. Fails if `x` cannot be converted to a string (a number can).
[uset](unicode.md#uset) reads a string's characters as UTF-8 instead.

```icon
procedure main()
   write(image(cset("hello")))
   write(image(cset(2026)))
   write(cset([]) | "no")
end
```

Output:

```
'ehlo'
'026'
no
```

**See also:** [string](#string), [uset](unicode.md#uset).

## function

```
function()
```

Generates the names of the built-in functions, in sorted order (those
beginning with a capital, the graphics functions, first).

```icon
procedure main()
   every writes(function() \ 3, " ")
   write()
end
```

Output:

```
Active Alert Bg 
```

**See also:** [proc](#proc).

## image

```
image(x)
```

Produces a string that shows `x`: for a string or cset, a literal that
would produce it (with escapes for bytes that are not printable, except
that valid, printable UTF-8 shows as itself); for a number, its usual
form; for a structure, its type, serial number and size
(`list_1(3)`); for a procedure, function or record constructor, what it
is and its name; for a keyword value such as `&null` or `&output`, the
keyword.

```icon
record point(x, y)
procedure main()
   write(image("tab\there"), " ", image('ab'), " ", image(&null))
   write(image([1, 2]), " ", image(point(1, 2)), " ", image(point))
   write(image(write), " ", image(main), " ", image(1e20))
end
```

Output:

```
"tab\there" 'ab' &null
list_1(2) record point_1(2) record constructor point
function write procedure main 1e+20
```

**See also:** [type](#type), [string](#string).

## integer

```
integer(x)
```

Produces `x` converted to an integer; fails if it cannot be. A string
may be an integer literal, with a radix (`"16r1F"`) and surrounding
spaces; a real, or a string that is one, is truncated toward zero.

```icon
procedure main()
   write(integer("12"), " ", integer(" 16r1F "), " ", integer("3.7"), " ", integer(-2.9))
   write(integer("twelve") | "no")
end
```

Output:

```
12 31 3 -2
no
```

**See also:** [numeric](#numeric), [real](#real).

## name

```
name(v)
```

Produces the name of the variable `v`: an identifier, a keyword
(`"&pos"`), a field with its record type (`"point.x"`), or a subscripted
name (`"L[1]"`).

**Errors:** 111 if `v` is not a variable.

```icon
record point(x, y)
global total
procedure main()
   r := point(1, 2)
   L := [1]
   write(name(total), " ", name(r.x), " ", name(L[1]), " ", name(&pos))
end
```

Output:

```
total point.x L[1] &pos
```

**See also:** [variable](#variable).

## numeric

```
numeric(x)
```

Produces `x` converted to a number, an integer if it can be one and a
real otherwise; fails if it cannot be converted.

```icon
procedure main()
   write(numeric("12"), " ", type(numeric("12")), " ", numeric("1.5"))
   write(numeric("x") | "no")
end
```

Output:

```
12 integer 1.5
no
```

**See also:** [integer](#integer), [real](#real).

## proc

```
proc(x, i)
```

Produces the procedure, function or record constructor `x` names.
If `x` is one already, `x` itself; if it is a string, the procedure,
built-in function or record constructor of that name. With `i` 1, 2 or
3, a string names an operator with that many operands (`proc("-", 1)`
is negation, and `proc("...", 3)` is `i to j by k`); with `i` 0, the
built-in function of that name, even where a procedure of the same name
hides it. Fails if there is none.

**Defaults:** `i` 1.

**Errors:** 101 if `i` is not an integer; 205 if it is not from 0 to 3.

```icon
procedure main()
   write(image(proc("left")), " ", image(proc("-", 1)), " ", image(proc("...", 3)))
   write(proc("+", 2)(3, 4))
   write(proc("nosuch") | "no")
end
```

Output:

```
function left function - function ...
7
no
```

**See also:** [args](#args), [function](#function).

## real

```
real(x)
```

Produces `x` converted to a real number; fails if it cannot be.

```icon
procedure main()
   write(real(2), " ", real("1e3"), " ", real(" .5 "))
   write(real("x") | "no")
end
```

Output:

```
2.0 1000.0 0.5
no
```

**See also:** [integer](#integer), [numeric](#numeric).

## serial

```
serial(x)
```

Produces the serial number of the structure, co-expression or window
`x`: the values of each type are numbered from 1 as they are made.
Fails for any other value.

```icon
procedure main()
   L := []
   write(serial(L), " ", serial([]), " ", serial(table()))
   write(serial(3) | "no")
end
```

Output:

```
1 2 1
no
```

**See also:** [image](#image).

## string

```
string(x)
```

Produces `x` converted to a string: a number in its usual form, a cset
as its characters in order. Fails if it cannot be converted.

```icon
procedure main()
   write(string(12), " ", string(1.5), " ", string('cba'))
   write(string([]) | "no")
end
```

Output:

```
12 1.5 abc
no
```

**See also:** [image](#image), [integer](#integer).

## type

```
type(x)
```

Produces the name of the type of `x`: `"integer"`, `"real"`,
`"string"`, `"cset"`, `"uset"`, `"null"`, `"list"`, `"table"`, `"set"`,
`"procedure"` (also for functions and record constructors),
`"co-expression"`, `"file"`, `"window"`, or for a record its record
type's name.

```icon
record point(x, y)
procedure main()
   every writes(type(1 | 1.0 | "s" | 'c' | u'c' | &null | [] | point(1, 2) | write), " ")
   write()
end
```

Output:

```
integer real string cset uset null list point procedure 
```

**See also:** [image](#image).

## variable

```
variable(s)
```

Produces the variable named `s`: a local of the procedure that calls
`variable`, a global, or a keyword. Fails if there is no such variable.

**Errors:** 103 if `s` is not a string.

```icon
global total
procedure main()
   local count
   count := 1
   variable("count") +:= 1
   variable("total") := 10
   write(count, " ", total)
   write(variable("nosuch") | "no")
end
```

Output:

```
2 10
no
```

**See also:** [name](#name).
