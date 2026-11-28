# Strings and Unicode

Strings are bytes, and csets are sets of the 256 byte values, exactly as
in Icon 9.5 (and as strings are in Go). Every string operation reads and
counts bytes, so text, binary data and Latin-1 behave as under iconx.
Positions, subscripts and `*s` are always in bytes.

Unicode is kept to one addition, the **uset**, and everything
Unicode-aware follows from it.

## Usets

- **A uset** is a set of characters for UTF-8 text: the ASCII
  characters, the 128 bytes from `\x80`, and every Unicode code point.
  It is written `u'...'`, with ranges as in csets (`u'é'`, `u'α..ω'`);
  `uset(x)` converts a string (read as UTF-8) or a cset, and `&uset` is
  every character.
- **Scanning with a uset reads UTF-8.** Given a uset, `any`, `many`,
  `upto`, `bal` and `trim` step over whole UTF-8 characters; given a
  cset, they step over bytes, as in 9.5. So `tab(any(&uset))` is the
  next character of the subject, and `tab(many(c))` with a cset is
  exactly what it always was. Positions stay byte positions.
- **Csets and usets never mix.** There is no silent conversion between
  them, as there is none between types in Go: `c ++ u`, `cset(u)`, or a
  cset and a uset in one `bal` is run-time error 128 (Gcon's), and
  `gcon vet` reports the mixes it can see. `uset(c)` is the one
  conversion.

```icon
link uclass

procedure main()
   letters := uclass("L")
   "naïve café, 1 €" ? while tab(upto(letters)) do
      write(tab(many(letters)))
end
```

Output:

```
naïve
café
```

The letters, each a whole UTF-8 character: the euro sign and the digit
are not letters.

## Code points

- `uord(s)` is the code point of `s`, which must be one UTF-8
  character: `uord("é")` is 233.
- `uchar(i)` is the UTF-8 encoding of the code point `i`: `uchar(233)`
  is `"é"`, two bytes. `char(i)` keeps its 9.5 meaning, one byte.
- `uvalid(s)` produces `s` if it is valid UTF-8, and fails otherwise.

A byte that is not part of valid UTF-8 is read as Go reads it: in a
uset it is that byte, and `uord` gives U+FFFD.

## Character classes

Unicode character classes are a library module, not language: `link
uclass`, and `uclass("L")`, `uclass("Nd")` or `uclass("Greek")` is that
general category or script as a uset, from the Unicode Character
Database the root was built with (Unicode 17.0.0). The names are those
of Go's `unicode` package; [the reference](ref/uclass.md) lists them all.

## Literals

- **Ranges** in cset and uset literals: `'a..zA..Z_'`, `'\x00..\x1f'`; in
  a cset both ends are single bytes.
- **`\u` and `\U` escapes** in any literal, after Go's, give a code
  point's UTF-8 bytes: `"caf\u00e9"` is the same 5 bytes as `"café"` in
  a UTF-8 source file. Each takes a fixed number of hex digits, so that
  what follows is never read as part of it: `\u` exactly 4, `\U` exactly
  6 (any code point needs no more). Go's 8-digit `"\U0001f600"` is
  U+01F6 followed by `00`; fewer digits are a compile-time error.

```icon
procedure main()
   s := "caf\u00e9"
   write(s, ", ", *s, " bytes")
   if s == "café" then write("the same bytes as \"café\"")
   write(image("\U01f600"), ", ", *"\U01f600", " bytes")
end
```

Output:

```
café, 5 bytes
the same bytes as "café"
"😀", 4 bytes
```

## image

One change shows in 9.5 programs' output: **`image`** shows a valid,
printable UTF-8 sequence as itself, so `image("café")` is `"café"`,
where iconx writes `"caf\xc3\xa9"`. Both are literals of the same
bytes, and no output in Icon's test suite changes.
