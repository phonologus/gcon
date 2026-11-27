# The reference

The built-in functions and keywords, as Gcon gives them. `gcon doc
NAME` shows one entry: `gcon doc find`, `gcon doc '&subject'`.

## The pages

- [Strings](strings.md): `center`, `char`, `detab`, `entab`, `left`, `map`,
  `ord`, `repl`, `reverse`, `right`, `trim`
- [String scanning](scanning.md): `any`, `bal`, `find`, `many`, `match`,
  `move`, `pos`, `tab`, `upto`
- [Unicode](unicode.md): `uchar`, `uord`, `uset`, `uvalid` (Gcon's)
- [Unicode character classes](uclass.md): `uclass` and its categories and
  scripts (Gcon's library module)
- [Values and types](values.md): `args`, `copy`, `cset`, `function`,
  `image`, `integer`, `name`, `numeric`, `proc`, `real`, `serial`,
  `string`, `type`, `variable`
- [Structures](structures.md): `delete`, `get`, `insert`, `key`, `list`,
  `member`, `pop`, `pull`, `push`, `put`, `set`, `sort`, `sortf`, `table`
- [Numbers](numbers.md): `abs`, the trigonometric functions, `exp`, `log`,
  `sqrt`, the bit operations (`iand`, `ior`, `ixor`, `icom`, `ishift`),
  `seq`
- [Files](files.md): `open`, `close`, `read`, `reads`, `write`, `writes`,
  `seek`, `where`, `flush`, `remove`, `rename`, `chdir`
- [The program and its system](system.md): `exit`, `stop`, `runerr`,
  `errorclear`, `display`, `system`, `getenv`, `delay`, `collect`,
  `loadfunc`, the keyboard (`getch`, `getche`, `kbhit`)
- [Keywords](keywords.md): every keyword but the graphics keywords
- [Graphics](graphics.md): opening windows, the attributes, the graphics
  functions and keywords

## Reading an entry

An entry begins with a synopsis, whose argument names say what each
argument is converted to:

| Name | Is |
|---|---|
| `s`, `s1`, ... | a string |
| `c` | a cset (in Gcon, a uset where the entry says so) |
| `i`, `j`, `i1`, ... | an integer |
| `r` | a real |
| `n` | a number (integer or real) |
| `x` | any value |
| `L` | a list |
| `T` | a table |
| `S` | a set |
| `R` | a record |
| `C` | a co-expression |
| `f` | a file |

Then what it does: whether it **produces** one result, can fail, or
**generates** a sequence of results. **Defaults** gives the value an
omitted (or `&null`) argument takes; **Errors** the run-time errors an
argument of the wrong type raises. An example shows the entry at work,
with the output it gives: `gcon doc -test` runs every example and
checks that output, so the entries are verified as the code is.

Positions in strings are between characters, from 1 before the first
to `*s + 1` after the last; 0 and negative positions count from the
end (0 is after the last character). A range `s[i1:i2]` is taken in
either order.
