# String scanning

The functions that analyse a string, and the scanning functions that
move through `&subject`. A function that analyses a string takes it,
and the range to look in, as its last three arguments; omitting the
string means `&subject` from `&pos`, so in a scanning expression they
look at the rest of the subject.

## find

```
find(s1, s2, i1, i2)
```

Generates the positions in `s2[i1:i2]` at which `s1` begins, from left
to right; fails if there are none.

**Defaults:** `s2` `&subject`; `i1` `&pos` if `s2` is defaulted, else 1;
`i2` 0.

**Errors:** 101 if `i1` or `i2` is not an integer; 103 if `s1` or `s2`
is not a string.

```icon
procedure main()
   every writes(find("an", "banana"), " ")
   write()
   "banana" ? {
      move(3)
      every writes(find("a"), " ")
   }
   write()
end
```

Output:

```
2 4 
4 6 
```

**See also:** [match](#match), [upto](#upto).

## many

```
many(c, s, i1, i2)
```

Produces the position after the longest run of characters in `c` that
starts at `i1` in `s[i1:i2]`; fails if the character at `i1` is not in
`c`. Given a uset, `many` steps over whole UTF-8 characters.

**Defaults:** `s` `&subject`; `i1` `&pos` if `s` is defaulted, else 1;
`i2` 0.

**Errors:** 101 if `i1` or `i2` is not an integer; 103 if `s` is not a
string; 104 if `c` is not a cset.

```icon
procedure main()
   write(many('ab', "abba cab"))
   "hello world" ? write(tab(many(&letters)))
end
```

Output:

```
5
hello
```

**See also:** [tab](#tab), [upto](#upto).

## match

```
match(s1, s2, i1, i2)
```

Produces the position after `s1` if `s2[i1:i2]` begins with `s1`; fails
otherwise.

**Defaults:** `s2` `&subject`; `i1` `&pos` if `s2` is defaulted, else 1;
`i2` 0.

**Errors:** 101 if `i1` or `i2` is not an integer; 103 if `s1` or `s2`
is not a string.

```icon
procedure main()
   write(match("ban", "banana"))
   write(match("nan", "banana") | "no match")
end
```

Output:

```
4
no match
```

**See also:** [find](#find), [tab](#tab).

## move

```
move(i)
```

Moves `&pos` by `i` characters and produces the part of `&subject` it
moved over; fails, leaving `&pos` as it was, if that would go outside
the subject. If the expression it is in fails afterwards, `&pos` is
restored.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   "hello" ? {
      write(move(2))
      write(move(-1))
      write(move(10) | "too far")
      write(&pos)
   }
end
```

Output:

```
he
e
too far
2
```

**See also:** [tab](#tab).

## tab

```
tab(i)
```

Sets `&pos` to `i` and produces the part of `&subject` between the old
and the new position; fails if `i` is not a position in the subject. If
the expression it is in fails afterwards, `&pos` is restored.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   "hello world" ? {
      write(tab(upto(' ')))
      move(1)
      write(tab(0))
   }
end
```

Output:

```
hello
world
```

**See also:** [move](#move), [many](#many), [upto](#upto).

## upto

```
upto(c, s, i1, i2)
```

Generates the positions in `s[i1:i2]` of the characters that are in
`c`, from left to right; fails if there are none. Given a uset, `upto`
reads whole UTF-8 characters.

**Defaults:** `s` `&subject`; `i1` `&pos` if `s` is defaulted, else 1;
`i2` 0.

**Errors:** 101 if `i1` or `i2` is not an integer; 103 if `s` is not a
string; 104 if `c` is not a cset.

```icon
procedure main()
   every writes(upto('aeiou', "education"), " ")
   write()
end
```

Output:

```
1 3 5 7 8 
```

**See also:** [find](#find), [many](#many), [tab](#tab).
