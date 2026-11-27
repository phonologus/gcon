# Strings

The functions that build and rearrange strings. Strings are bytes, as in
Icon 9.5: lengths and positions count bytes, and a function that pads or
truncates does so by bytes. [Unicode](unicode.md) has the functions that
read UTF-8.

## center

```
center(s1, i, s2)
```

Produces a string of length `i` with `s1` in the middle, padded on both
sides with copies of `s2`; where the padding cannot be even, the extra
goes on the right. If `s1` is longer than `i`, the middle `i` bytes of
it, more being dropped on the left.

**Defaults:** `i` 1; `s2` `" "`.

**Errors:** 101 if `i` is not an integer; 103 if `s1` or `s2` is not a
string; 205 if `i` is negative.

```icon
procedure main()
   write("[", center("ab", 5), "]")
   write("[", center("title", 11, "-"), "]")
   write("[", center("abcdef", 3), "]")
end
```

Output:

```
[ ab  ]
[---title---]
[cde]
```

**See also:** [left](#left), [right](#right).

## char

```
char(i)
```

Produces the one-byte string whose byte is `i`, from 0 to 255.
[uchar](unicode.md#uchar) gives a code point's UTF-8 encoding instead.

**Errors:** 101 if `i` is not an integer; 205 if it is not from 0 to
255.

```icon
procedure main()
   write(char(65), char(98))
   write(image(char(0)), " ", image(char(233)))
end
```

Output:

```
Ab
"\x00" "\xe9"
```

**See also:** [ord](#ord), [uchar](unicode.md#uchar).

## detab

```
detab(s, i1, i2, ...)
```

Produces `s` with each tab replaced by the spaces that take it to the
next tab stop. The stops are at columns `i1`, `i2`, ... (column 1 is the
first byte), and after the last given, at the same interval as the last
two (or as `i1` is from column 1). A newline or return starts column 1
again; a backspace moves back a column.

**Defaults:** one stop, at column 9: tab stops every 8 columns.

**Errors:** 101 if a stop is not an integer; 103 if `s` is not a
string; 210 if a stop is not after column 1 and after the stop before
it.

```icon
procedure main()
   write(image(detab("a\tb\tc")))
   write(image(detab("a\tbc\td", 5)))
end
```

Output:

```
"a       b       c"
"a   bc  d"
```

**See also:** [entab](#entab).

## entab

```
entab(s, i1, i2, ...)
```

Produces `s` with runs of spaces replaced by tabs where a tab reaches the
same column, the tab stops given as for [detab](#detab). A single space
before a stop stays a space.

**Defaults:** one stop, at column 9: tab stops every 8 columns.

**Errors:** 101 if a stop is not an integer; 103 if `s` is not a
string; 210 if a stop is not after column 1 and after the stop before
it.

```icon
procedure main()
   write(image(entab("a       b")))
   write(image(entab("ab  cd", 3)))
end
```

Output:

```
"a\tb"
"ab\tcd"
```

**See also:** [detab](#detab).

## left

```
left(s1, i, s2)
```

Produces a string of length `i` with `s1` at the left, padded on the
right with copies of `s2` (aligned so that the last copy ends at the
right). If `s1` is longer than `i`, its first `i` bytes.

**Defaults:** `i` 1; `s2` `" "`.

**Errors:** 101 if `i` is not an integer; 103 if `s1` or `s2` is not a
string; 205 if `i` is negative.

```icon
procedure main()
   write("[", left("ab", 5, "."), "]")
   write("[", left("abcdef", 3), "]")
   write("[", left("a", 6, "xyz"), "]")
end
```

Output:

```
[ab...]
[abc]
[ayzxyz]
```

**See also:** [center](#center), [right](#right).

## map

```
map(s1, s2, s3)
```

Produces `s1` with each byte that occurs in `s2` replaced by the byte at
the same position in `s3`. Where a byte occurs more than once in `s2`,
its last occurrence decides. With `s1` a fixed pattern and `s2` the
string to rearrange, `map` permutes: `map("21", "12", "ab")` is `"ba"`.

**Defaults:** `s2` `&ucase`; `s3` `&lcase`: so `map(s)` is `s` in lower
case.

**Errors:** 103 if `s1`, `s2` or `s3` is not a string; 208 if `s2` and
`s3` differ in length.

```icon
procedure main()
   write(map("Hello, World"))
   write(map("hello", "lo", "01"))
   write(map("21", "12", "ab"))
end
```

Output:

```
hello, world
he001
ba
```

**See also:** [string](values.md#string).

## ord

```
ord(s)
```

Produces the value of the one byte of `s`, from 0 to 255.
[uord](unicode.md#uord) gives the code point of a UTF-8 character
instead.

**Errors:** 103 if `s` is not a string; 205 if it is not one byte long.

```icon
procedure main()
   write(ord("A"), " ", ord("\n"))
end
```

Output:

```
65 10
```

**See also:** [char](#char), [uord](unicode.md#uord).

## repl

```
repl(s, i)
```

Produces `i` copies of `s`, concatenated; `repl(s, 0)` is the empty
string.

**Errors:** 101 if `i` is not an integer; 103 if `s` is not a string;
205 if `i` is negative.

```icon
procedure main()
   write(repl("ab", 3))
   write(repl("-", 20))
end
```

Output:

```
ababab
--------------------
```

**See also:** [left](#left).

## reverse

```
reverse(s)
```

Produces `s` with its bytes in the opposite order. A multi-byte UTF-8
character is reversed byte by byte too, so the result of reversing
UTF-8 text is not UTF-8.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   write(reverse("stressed"))
end
```

Output:

```
desserts
```

## right

```
right(s1, i, s2)
```

Produces a string of length `i` with `s1` at the right, padded on the
left with copies of `s2` (aligned so that the first copy starts at the
left). If `s1` is longer than `i`, its last `i` bytes.

**Defaults:** `i` 1; `s2` `" "`.

**Errors:** 101 if `i` is not an integer; 103 if `s1` or `s2` is not a
string; 205 if `i` is negative.

```icon
procedure main()
   every write(right(1 to 3, 4, "0"))
   write("[", right("abcdef", 3), "]")
end
```

Output:

```
0001
0002
0003
[def]
```

**See also:** [center](#center), [left](#left).

## trim

```
trim(s, c)
```

Produces `s` without the characters in `c` at its end. Given a uset,
`trim` removes whole UTF-8 characters.

**Defaults:** `c` `' '`.

**Errors:** 103 if `s` is not a string; 104 if `c` is not a cset.

```icon
procedure main()
   write("[", trim("abc   "), "]")
   write("[", trim("12.5000", '0'), "]")
   write("[", trim("café", u'é'), "]")
end
```

Output:

```
[abc]
[12.5]
[caf]
```

**See also:** [left](#left).
