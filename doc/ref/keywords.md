# Keywords

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
