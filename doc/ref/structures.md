# Structures

Lists, sets and tables: making them, adding and removing elements, and
sorting. Records are made by their constructors (`point(1, 2)`); the
operators `*`, `!`, `?` and subscripting work on all of them.

## delete

```
delete(x1, x2)
```

Removes the member `x2` from the set `x1`, or the entry with key `x2`
from the table `x1`, if it is there, and produces `x1`.

**Errors:** 122 if `x1` is not a set or table.

```icon
procedure main()
   S := set(["a", "b", "c"])
   delete(S, "b")
   write(*S, " ", member(S, "b") | "gone")
end
```

Output:

```
2 gone
```

**See also:** [insert](#insert), [member](#member).

## get

```
get(L)
```

Removes the first element of the list `L` and produces it; fails if `L`
is empty. The same as [pop](#pop): with [put](#put), a queue.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   Q := []
   put(Q, "first", "second")
   write(get(Q), " ", get(Q))
   write(get(Q) | "empty")
end
```

Output:

```
first second
empty
```

**See also:** [pop](#pop), [pull](#pull), [put](#put).

## insert

```
insert(x1, x2, x3)
```

Adds `x2` to the set `x1`, or to the table `x1` an entry with key `x2`
and value `x3` (replacing the value of an entry already there), and
produces `x1`.

**Defaults:** `x3` `&null`.

**Errors:** 122 if `x1` is not a set or table.

```icon
procedure main()
   T := table()
   insert(T, "x", 10)
   insert(T, "x", 20)
   write(*T, " ", T["x"])
end
```

Output:

```
1 20
```

**See also:** [delete](#delete), [member](#member).

## key

```
key(T)
```

Generates the keys of the table `T`, in no particular order.

**Errors:** 124 if `T` is not a table.

```icon
procedure main()
   T := table()
   T["one"] := 1
   T["two"] := 2
   K := []
   every put(K, key(T))
   every k := !sort(K) do write(k, " ", T[k])
end
```

Output:

```
one 1
two 2
```

**See also:** [sort](#sort), [table](#table).

## list

```
list(i, x)
```

Produces a new list of `i` elements, each `x`.

**Defaults:** `i` 0; `x` `&null`.

**Errors:** 101 if `i` is not an integer; 205 if it is negative.

```icon
procedure main()
   L := list(3, 0)
   L[2] := 5
   every writes(!L, " ")
   write()
   write(*list())
end
```

Output:

```
0 5 0 
0
```

**See also:** [put](#put), [table](#table).

## member

```
member(x1, x2)
```

Produces `x2` if it is a member of the set `x1`, or a key of the table
`x1`; fails otherwise.

**Errors:** 122 if `x1` is not a set or table.

```icon
procedure main()
   S := set([1, 2, 3])
   write(member(S, 2))
   write(member(S, 5) | "not a member")
end
```

Output:

```
2
not a member
```

**See also:** [delete](#delete), [insert](#insert).

## pop

```
pop(L)
```

Removes the first element of the list `L` and produces it; fails if `L`
is empty. With [push](#push), a stack.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   stack := []
   push(stack, 1)
   push(stack, 2)
   write(pop(stack), " ", pop(stack))
end
```

Output:

```
2 1
```

**See also:** [get](#get), [push](#push).

## pull

```
pull(L)
```

Removes the last element of the list `L` and produces it; fails if `L`
is empty.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   L := [1, 2, 3]
   write(pull(L), " ", *L)
end
```

Output:

```
3 2
```

**See also:** [pop](#pop), [put](#put).

## push

```
push(L, x1, x2, ...)
```

Adds `x1`, then `x2`, and so on, to the front of the list `L`, and
produces `L`: the last given ends up first.

**Defaults:** `x1` `&null`.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   L := [1, 2]
   push(L, 3, 4)
   every writes(!L, " ")
   write()
end
```

Output:

```
4 3 1 2 
```

**See also:** [pop](#pop), [put](#put).

## put

```
put(L, x1, x2, ...)
```

Adds `x1`, `x2`, ... to the end of the list `L`, in order, and produces
`L`.

**Defaults:** `x1` `&null`.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   L := [1, 2]
   put(L, 3, 4)
   every writes(!L, " ")
   write()
end
```

Output:

```
1 2 3 4 
```

**See also:** [get](#get), [pull](#pull), [push](#push).

## set

```
set(L)
```

Produces a new set of the elements of the list `L`, each once.

**Defaults:** `L` an empty list.

**Errors:** 108 if `L` is not a list.

```icon
procedure main()
   S := set([3, 1, 3, 2, 1])
   write(*S)
   every writes(!sort(S), " ")
   write()
end
```

Output:

```
3
1 2 3 
```

**See also:** [insert](#insert), [member](#member).

## sort

```
sort(x, i)
```

Produces a new list of the elements of `x` (a list, set or record)
sorted, or of the entries of the table `x`. Values of different types
sort in the order `&null`, integers, reals, strings, csets, then
files, co-expressions, procedures, lists, sets, tables, records; numbers by
value, strings and csets byte by byte, and structures of a type by
serial number. A table sorts according to `i`: 1, a list of two-element
lists `[key, value]` sorted by key; 2, the same sorted by value; 3, a
list of keys and values alternating, sorted by key; 4, the same sorted
by value.

**Defaults:** `i` 1.

**Errors:** 101 if `i` is not an integer; 115 if `x` is not a
structure; 205 if `i` is not from 1 to 4.

```icon
procedure main()
   every writes(image(!sort([3, "a", 1.5, 2, &null, "B"])), " ")
   write()
   T := table()
   T["b"] := 1
   T["a"] := 2
   every p := !sort(T) do writes(p[1], "=", p[2], " ")
   write()
   every writes(!sort(T, 4), " ")
   write()
end
```

Output:

```
&null 2 3 1.5 "B" "a" 
a=2 b=1 
b 1 a 2 
```

**See also:** [sortf](#sortf).

## sortf

```
sortf(x, i)
```

Produces a new list of the elements of the list or set `x`, sorted by
their `i`-th field: records by field `i`, lists by element `i`
(counting from the end if `i` is negative).

**Defaults:** `i` 1.

**Errors:** 101 if `i` is not an integer; 125 if `x` is not a list or
set; 205 if `i` is 0.

```icon
record point(x, y)
procedure main()
   pts := [point(3, "c"), point(1, "b"), point(2, "a")]
   every writes((!sortf(pts, 1)).x, " ")
   write()
   every writes((!sortf(pts, 2)).y, " ")
   write()
end
```

Output:

```
1 2 3 
a b c 
```

**See also:** [sort](#sort).

## table

```
table(x)
```

Produces a new, empty table whose default value is `x`: the value of
`T[k]` for a key not in it.

**Defaults:** `x` `&null`.

```icon
procedure main()
   count := table(0)
   every count[!"banana"] +:= 1
   every p := !sort(count) do writes(p[1], p[2], " ")
   write()
end
```

Output:

```
a3 b1 n2 
```

**See also:** [insert](#insert), [key](#key), [sort](#sort).
