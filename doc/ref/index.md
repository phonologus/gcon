# The reference

The built-in functions and keywords, as Gcon gives them. `gcon doc
NAME` shows one entry: `gcon doc find`, `gcon doc '&subject'`.

## Pages

- [Strings](strings.md): `center`, `char`, `detab`, `entab`, `left`, `map`,
  `ord`, `repl`, `reverse`, `right`, `trim`
- [String scanning](scanning.md): `any`, `bal`, `find`, `many`, `match`,
  `move`, `pos`, `tab`, `upto`
- [Unicode](unicode.md): `uchar`, `uord`, `uset`, `uvalid` (Gcon's)
- [Values and types](values.md): `args`, `copy`, `cset`, `function`,
  `image`, `integer`, `name`, `numeric`, `proc`, `real`, `serial`,
  `string`, `type`, `variable`
- [Structures](structures.md): `delete`, `get`, `insert`, `key`, `list`,
  `member`, `pop`, `pull`, `push`, `put`, `set`, `sort`, `sortf`, `table`
- [Keywords](keywords.md): `&pos`, `&subject`, `&uset`, ...

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
