# Numbers

Arithmetic, trigonometry, logarithms, bit operations on integers, and
sequences. Integers have no size limit; reals are IEEE doubles. Angles
are in radians.

## abs

```
abs(n)
```

Produces the absolute value of the number `n`, of the same type.

**Errors:** 102 if `n` is not a number.

```icon
procedure main()
   write(abs(-3), " ", abs(-2.5), " ", abs(-2^70))
end
```

Output:

```
3 2.5 1180591620717411303424
```

## acos

```
acos(r)
```

Produces the angle, from 0 to pi, whose cosine is `r`.

**Errors:** 102 if `r` is not a number; 205 if it is not from -1 to 1.

```icon
procedure main()
   write(acos(1), " ", acos(-1))
end
```

Output:

```
0.0 3.141592654
```

**See also:** [cos](#cos).

## asin

```
asin(r)
```

Produces the angle, from -pi/2 to pi/2, whose sine is `r`.

**Errors:** 102 if `r` is not a number; 205 if it is not from -1 to 1.

```icon
procedure main()
   write(asin(1))
end
```

Output:

```
1.570796327
```

**See also:** [sin](#sin).

## atan

```
atan(r1, r2)
```

Produces the angle whose tangent is `r1`, from -pi/2 to pi/2; given
`r2`, the angle of the point (`r2`, `r1`) from the x axis, from -pi to
pi, so that the quadrant is right.

**Defaults:** `r2` 1.0.

**Errors:** 102 if `r1` or `r2` is not a number.

```icon
procedure main()
   write(atan(1), " ", atan(1, -1), " ", atan(0, -1))
end
```

Output:

```
0.7853981634 2.35619449 3.141592654
```

**See also:** [tan](#tan).

## cos

```
cos(r)
```

Produces the cosine of the angle `r`.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(cos(0), " ", cos(&pi))
end
```

Output:

```
1.0 -1.0
```

**See also:** [acos](#acos), [sin](#sin).

## dtor

```
dtor(r)
```

Produces the angle `r` degrees, in radians.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(dtor(180), " ", sin(dtor(90)))
end
```

Output:

```
3.141592654 1.0
```

**See also:** [rtod](#rtod).

## exp

```
exp(r)
```

Produces e raised to the power `r`.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(exp(1), " ", exp(0))
end
```

Output:

```
2.718281828 1.0
```

**See also:** [log](#log).

## iand

```
iand(i1, i2)
```

Produces the bitwise and of `i1` and `i2`, as two's complement integers
of any size.

**Errors:** 101 if `i1` or `i2` is not an integer.

```icon
procedure main()
   write(iand(12, 10), " ", iand(-1, 255))
end
```

Output:

```
8 255
```

**See also:** [ior](#ior), [ixor](#ixor), [icom](#icom).

## icom

```
icom(i)
```

Produces the bitwise complement of `i`: `-i - 1`.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   write(icom(0), " ", icom(5))
end
```

Output:

```
-1 -6
```

**See also:** [iand](#iand).

## ior

```
ior(i1, i2)
```

Produces the bitwise inclusive or of `i1` and `i2`.

**Errors:** 101 if `i1` or `i2` is not an integer.

```icon
procedure main()
   write(ior(12, 10))
end
```

Output:

```
14
```

**See also:** [iand](#iand), [ixor](#ixor).

## ishift

```
ishift(i, j)
```

Produces `i` shifted `j` bits: left if `j` is positive, right if it is
negative. A right shift keeps the sign, so it divides and rounds down.

**Errors:** 101 if `i` or `j` is not an integer.

```icon
procedure main()
   write(ishift(1, 4), " ", ishift(16, -2), " ", ishift(-16, -2), " ", ishift(1, 70))
end
```

Output:

```
16 4 -4 1180591620717411303424
```

**See also:** [iand](#iand).

## ixor

```
ixor(i1, i2)
```

Produces the bitwise exclusive or of `i1` and `i2`.

**Errors:** 101 if `i1` or `i2` is not an integer.

```icon
procedure main()
   write(ixor(12, 10))
end
```

Output:

```
6
```

**See also:** [iand](#iand), [ior](#ior).

## log

```
log(r1, r2)
```

Produces the logarithm of `r1` to the base `r2`.

**Defaults:** `r2` e, the natural logarithm.

**Errors:** 102 if `r1` or `r2` is not a number; 205 if `r1` is not
positive, or `r2` is not positive or is 1.

```icon
procedure main()
   write(log(&e), " ", log(8, 2), " ", log(100, 10))
end
```

Output:

```
1.0 3.0 2.0
```

**See also:** [exp](#exp).

## rtod

```
rtod(r)
```

Produces the angle `r` radians, in degrees.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(rtod(&pi))
end
```

Output:

```
180.0
```

**See also:** [dtor](#dtor).

## seq

```
seq(i, j)
```

Generates `i`, `i + j`, `i + 2 * j`, ... without end.

**Defaults:** `i` 1; `j` 1.

**Errors:** 101 if `i` or `j` is not an integer; 211 if `j` is 0.

```icon
procedure main()
   every writes(seq() \ 3, " ")
   every writes(seq(10, -3) \ 4, " ")
   write()
end
```

Output:

```
1 2 3 10 7 4 1 
```

**See also:** the operator `i to j by k`.

## sin

```
sin(r)
```

Produces the sine of the angle `r`.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(sin(&pi / 2), " ", sin(0))
end
```

Output:

```
1.0 0.0
```

**See also:** [asin](#asin), [cos](#cos).

## sqrt

```
sqrt(r)
```

Produces the square root of `r`, a real.

**Errors:** 102 if `r` is not a number; 205 if it is negative.

```icon
procedure main()
   write(sqrt(16), " ", sqrt(2))
end
```

Output:

```
4.0 1.414213562
```

## tan

```
tan(r)
```

Produces the tangent of the angle `r`.

**Errors:** 102 if `r` is not a number.

```icon
procedure main()
   write(tan(0), " ", tan(&pi / 4))
end
```

Output:

```
0.0 1.0
```

**See also:** [atan](#atan).
