# Unicode

Gcon's additions for UTF-8 text, which Icon 9.5 lacks
([Strings and Unicode](../unicode.md) explains them). Strings stay
bytes; a uset is what reads UTF-8, in the scanning functions
([any](scanning.md#any), [bal](scanning.md#bal),
[many](scanning.md#many), [upto](scanning.md#upto)) and in
[trim](strings.md#trim). `gcon vet -icon95` reports their use, for code
that must also run under Arizona Icon. The Unicode character classes are
a library module: `link uclass`, then `uclass("L")` or
`uclass("Greek")` is a uset.

## uchar

```
uchar(i)
```

Produces the UTF-8 encoding of the code point `i`: one to four bytes.
A value that is not a code point (negative, a surrogate, or above
0x10FFFF) gives U+FFFD, the replacement character. [char](strings.md#char)
keeps its Icon 9.5 meaning, one byte.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   write("h", uchar(233), "llo")
   write(*uchar(233), " ", *uchar(128512))
end
```

Output:

```
héllo
2 4
```

**See also:** [uord](#uord), [char](strings.md#char).

## uord

```
uord(s)
```

Produces the code point of `s`, which must be one UTF-8 character. A
byte that is not part of valid UTF-8 gives 65533 (U+FFFD), as Go reads
it; [ord](strings.md#ord) gives a byte's value.

**Errors:** 103 if `s` is not a string; 205 if it is not one character.

```icon
procedure main()
   write(uord("é"), " ", uord("A"))
   write(uord("\xff"))
end
```

Output:

```
233 65
65533
```

**See also:** [uchar](#uchar), [ord](strings.md#ord).

## uset

```
uset(x)
```

Produces `x` as a uset, a set of characters drawn from the ASCII
characters, the 128 bytes from 0x80 and every Unicode code point. A
string's characters are read as UTF-8 (a byte that is not part of valid
UTF-8 is that byte); a cset converts exactly. Fails if `x` cannot be
converted to a string. A uset literal is written `u'...'`, with ranges
(`u'α..ω'`), and `~`, `++`, `--`, `**` and `*` work on usets as on
csets; a cset and a uset never mix (run-time error 128).

```icon
procedure main()
   u := uset("héllo")
   write(image(u), " ", *u)
   write(image(uset('abc') ++ u'é'))
end
```

Output:

```
u'hloé' 4
u'abcé'
```

**See also:** [&uset](keywords.md#uset), [cset](values.md#cset).

## uvalid

```
uvalid(s)
```

Produces `s` if it is valid UTF-8; fails otherwise.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   write(uvalid("café"))
   write(uvalid("caf\xe9") | "not UTF-8")
end
```

Output:

```
café
not UTF-8
```

**See also:** [uord](#uord).
