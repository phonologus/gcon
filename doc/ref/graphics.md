# Graphics

The graphics functions and keywords. [Graphics](../graphics.md) describes
Gcon's display server, fonts and images; the IPL's `graphics` module
(`link graphics`) adds procedures on top of these, such as `WOpen`.

Every function here takes a window as its first argument, which may be
left out: then `&window` is used. A drawing function produces the
window. The errors common to all: 140 if there is no window (`&window`
is `&null` and none is given); 142 if the window has been closed; 145
for an attribute that does not exist; 146 if a function that takes
coordinates in groups is given an incomplete group. Coordinates are
pixels from (0, 0) at the top left, relative to the window's `dx` and
`dy`; colours are names (`"red"`, `"light blue"`), `"#rrggbb"` (or
`#rgb`, `#rrrrggggbbbb`), or three numbers `"r,g,b"` from 0 to 65535.
(`WinButton`, `WinColorDialog` and the other `Win` functions of Icon 9.5
on Windows are not provided.)

## Opening a window

`open(s, "g", attribute, ...)` opens a window labelled `s`, with the
attributes given as strings `"name=value"`; mode `"x"` is the same. It
does not set `&window` (the IPL's `WOpen` does). A window with
`canvas=hidden` is never shown, and needs no display: the examples below
use one.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=100,80") | stop("no window")
   write(image(w), " ", type(w), " ", WAttrib(w, "size"))
   close(w)
end
```

Output:

```
window_1:1(demo) window 100,80
```

## Window attributes

A window's attributes are read and set with [WAttrib](#wattrib), given
to `open`, and given to [Clone](#clone). Some belong to the canvas, shared
by every window on it; the rest belong to the context, the window's own.

| Attribute | Value |
|---|---|
| `label`, `windowlabel` | the window's title |
| `size`, `width`, `height` | its size in pixels, `"w,h"` |
| `pos`, `posx`, `posy` | its position on the screen |
| `geometry` | its size and position, `"wxh+x+y"` |
| `canvas` | `normal`, `hidden`, `iconic` or `maximal` |
| `resize` | `on` if the user may resize it |
| `display`, `depth`, `displaywidth`, `displayheight` | the display |
| `fg`, `bg` | the foreground and background colours |
| `reverse` | `on` to exchange `fg` and `bg` |
| `drawop` | how drawing combines with what is there: `copy`, `reverse`, or one of `and`, `andInverted`, `andReverse`, `clear`, `copyInverted`, `equiv`, `invert`, `nand`, `noop`, `nor`, `or`, `orInverted`, `orReverse`, `set`, `xor` |
| `linewidth`, `linestyle` | lines' width, and `solid`, `dashed` or `striped` |
| `fillstyle`, `pattern` | `solid`, `masked` or `textured` filling with a pattern |
| `clipx`, `clipy`, `clipw`, `cliph` | the clipping rectangle |
| `dx`, `dy` | the origin's offset |
| `font`, `ascent`, `descent`, `fwidth`, `fheight`, `leading` | the font and its metrics |
| `x`, `y`, `row`, `col`, `rows`, `columns`, `lines` | the text cursor, and the window's size in text |
| `echo`, `cursor` | whether typed characters echo, and the text cursor is shown |
| `pointer`, `pointerx`, `pointery`, `pointerrow`, `pointercol` | the mouse pointer |
| `image` | an image file to load into the canvas |
| `iconimage`, `iconlabel`, `iconpos`, `iconic`, `gamma`, `visual`, `ceol` | accepted, as Icon 9.5's, though some do nothing here |

## Active

```
Active()
```

Produces a window that has an event waiting, waiting until one has;
fails if no window is open. Windows are taken in turn, so that none is
starved.

```icon
procedure main()
   w := Active()
   write("an event in ", image(w))
end
```

This example is only translated: it waits for the user.

**See also:** [Event](#event), [Pending](#pending).

## Alert

```
Alert(w, i)
```

Sounds the alert (the system's beep) at volume `i`, and produces `w`.

**Defaults:** `i` 0.

**Errors:** 101 if `i` is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(Alert(w)))
end
```

Output:

```
window_1:1(demo)
```

## Bg

```
Bg(w, s)
```

Produces the background colour of `w`, after setting it to `s` if `s`
is given; fails if `s` is not a colour.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(Bg(w), " ", Bg(w, "light blue"), " ", Bg(w, "no such colour") | "no")
end
```

Output:

```
white light blue no
```

**See also:** [Fg](#fg), [EraseArea](#erasearea).

## Clip

```
Clip(w, x, y, i1, i2)
```

Limits drawing in `w` to the rectangle at (`x`, `y`) of width `i1` and
height `i2`, and produces `w`; with no rectangle, drawing is no longer
limited.

**Errors:** 101 if a coordinate is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   Clip(w, 0, 0, 10, 10)
   FillRectangle(w)
   write(Pixel(w, 5, 5, 1, 1), " ", Pixel(w, 20, 20, 1, 1))
   Clip(w)
   write(image(WAttrib(w, "clipw")))
end
```

Output:

```
65535,0,0 65535,65535,65535
&null
```

## Clone

```
Clone(w, s1, s2, ...)
```

Produces a new window on the canvas of `w`, with a context of its own: a
copy of `w`'s, then the attributes `s1`, `s2`, ... set. What it draws
appears on the same canvas.

**Errors:** 109 if an attribute is not a string; 145 if one does not
exist.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50")
   c := Clone(w, "fg=red")
   FillRectangle(c, 0, 0, 10, 10)
   write(image(c), " ", Fg(c), " ", Fg(w), " ", Pixel(w, 5, 5, 1, 1))
end
```

Output:

```
window_1:2(demo) red black 65535,0,0
```

**See also:** [Couple](#couple).

## Color

```
Color(w, i, s, ...)
```

Sets or produces the colour of mutable colour `i`. Gcon has no mutable
colours (as an X server with true colour has none), so `Color` fails.

**Errors:** 101 if `i` is not an integer; 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(Color(w, 1) | "no mutable colours")
end
```

Output:

```
no mutable colours
```

**See also:** [NewColor](#newcolor).

## ColorValue

```
ColorValue(w, s)
```

Produces the colour `s` as three numbers from 0 to 65535, `"r,g,b"`;
fails if `s` is not a colour.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(ColorValue(w, "red"), " ", ColorValue(w, "#00ff00"), " ", ColorValue(w, "light blue"))
   write(ColorValue(w, "no such colour") | "no")
end
```

Output:

```
65535,0,0 0,65535,0 21845,21845,65535
no
```

## CopyArea

```
CopyArea(w1, w2, x1, y1, i1, i2, x2, y2)
```

Copies the rectangle at (`x1`, `y1`), of width `i1` and height `i2`, of
the window `w1` to (`x2`, `y2`) in `w2`, and produces `w1`. With one
window, it copies within it.

**Defaults:** `w2` `w1`; the rectangle the whole window; `x2`, `y2` 0.

**Errors:** 101 if a coordinate is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=60,60", "fg=red")
   FillRectangle(w, 0, 0, 4, 4)
   CopyArea(w, 0, 0, 4, 4, 50, 50)
   write(Pixel(w, 51, 51, 1, 1))
end
```

Output:

```
65535,0,0
```

## Couple

```
Couple(w1, w2)
```

Produces a new window with the canvas of `w1` and the context of `w2`:
drawing in it draws on `w1`'s canvas with `w2`'s colours, font and so on.

**Errors:** 140 if `w1` or `w2` is not a window.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50")
   red := Clone(w, "fg=red")
   k := Couple(w, red)
   FillRectangle(k, 0, 0, 5, 5)
   write(Pixel(w, 2, 2, 1, 1))
end
```

Output:

```
65535,0,0
```

**See also:** [Clone](#clone), [Uncouple](#uncouple).

## DrawArc

```
DrawArc(w, x, y, i1, i2, r1, r2, ...)
```

Draws the arc of the ellipse that fits the rectangle at (`x`, `y`) of
width `i1` and height `i2`, from the angle `r1` (0 is to the right)
through the angle `r2`, in radians; and so on for each group of six.
Produces `w`.

**Defaults:** `r1` 0; `r2` 2 pi: the whole ellipse.

**Errors:** 101 if a coordinate is not an integer; 102 if an angle is
not a number; 146 if a group is incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   DrawArc(w, 10, 10, 20, 20)
   write(Pixel(w, 30, 20, 1, 1), " ", Pixel(w, 20, 20, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [DrawCircle](#drawcircle), [FillArc](#fillarc).

## DrawCircle

```
DrawCircle(w, x, y, r, r1, r2, ...)
```

Draws the circle (or the arc of it from angle `r1` through `r2`) with
centre (`x`, `y`) and radius `r`; and so on for each group of five.
Produces `w`.

**Defaults:** `r1` 0; `r2` 2 pi.

**Errors:** 101 if a coordinate is not an integer; 102 if the radius or
an angle is not a number; 146 if a group is incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   DrawCircle(w, 25, 25, 10)
   write(Pixel(w, 35, 25, 1, 1), " ", Pixel(w, 25, 25, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [FillCircle](#fillcircle).

## DrawCurve

```
DrawCurve(w, x1, y1, x2, y2, ...)
```

Draws a smooth curve through the points, and produces `w`. If the first
point and the last are the same, the curve is closed smoothly.

**Errors:** 101 if a coordinate is not an integer; 146 if a point is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red", "linewidth=3")
   DrawCurve(w, 5, 25, 25, 5, 45, 25)
   write(Pixel(w, 25, 5, 1, 1))
end
```

Output:

```
65535,0,0
```

**See also:** [DrawLine](#drawline).

## DrawImage

```
DrawImage(w, x, y, s)
```

Draws the image given by the string `s` with its top left at (`x`, `y`),
and produces `w`; fails if `s` is not an image. `s` is `"width,palette,
data"`, the data one character per pixel from the palette (`c1` to
`c6`, `g2` to `g256`), or `"width,#hex"`, a bitmap in hexadecimal whose
1 bits are drawn in the foreground colour (with `~` in place of `#`, the
0 bits in the background colour too).

**Defaults:** `x`, `y` 0.

**Errors:** 101 if `x` or `y` is not an integer; 103 if `s` is not a
string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=20,20")
   DrawImage(w, 0, 0, "3,g16,0F0F0F0F0")
   write(Pixel(w, 0, 0, 1, 1), " ", Pixel(w, 1, 0, 1, 1))
end
```

Output:

```
0,0,0 65535,65535,65535
```

**See also:** [PaletteChars](#palettechars), [ReadImage](#readimage).

## DrawLine

```
DrawLine(w, x1, y1, x2, y2, ...)
```

Draws lines joining the points in turn, and produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a point is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=blue")
   DrawLine(w, 0, 30, 49, 30)
   write(Pixel(w, 25, 30, 1, 1), " ", Pixel(w, 25, 31, 1, 1))
end
```

Output:

```
0,0,65535 65535,65535,65535
```

**See also:** [DrawSegment](#drawsegment), [DrawPolygon](#drawpolygon).

## DrawPoint

```
DrawPoint(w, x1, y1, x2, y2, ...)
```

Draws each point, and produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a point is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=20,20")
   DrawPoint(w, 5, 5, 6, 6)
   write(Pixel(w, 5, 5, 1, 1), " ", Pixel(w, 7, 7, 1, 1))
end
```

Output:

```
0,0,0 65535,65535,65535
```

## DrawPolygon

```
DrawPolygon(w, x1, y1, x2, y2, ...)
```

Draws the outline of the polygon with the points as its corners, the
last joined to the first, and produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a point is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   DrawPolygon(w, 10, 10, 40, 10, 25, 40)
   write(Pixel(w, 25, 10, 1, 1), " ", Pixel(w, 25, 20, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [FillPolygon](#fillpolygon).

## DrawRectangle

```
DrawRectangle(w, x, y, i1, i2, ...)
```

Draws the outline of the rectangle at (`x`, `y`), of width `i1` and
height `i2`; and so on for each group of four. Produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a group is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   DrawRectangle(w, 20, 20, 10, 10)
   write(Pixel(w, 20, 25, 1, 1), " ", Pixel(w, 25, 25, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [FillRectangle](#fillrectangle).

## DrawSegment

```
DrawSegment(w, x1, y1, x2, y2, ...)
```

Draws a line from (`x1`, `y1`) to (`x2`, `y2`), and so on for each group
of four: separate lines, not joined. Produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a group is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=20,20")
   DrawSegment(w, 0, 0, 10, 0, 0, 5, 10, 5)
   write(Pixel(w, 5, 0, 1, 1), " ", Pixel(w, 5, 5, 1, 1), " ", Pixel(w, 5, 3, 1, 1))
end
```

Output:

```
0,0,0 0,0,0 65535,65535,65535
```

**See also:** [DrawLine](#drawline).

## DrawString

```
DrawString(w, x, y, s, ...)
```

Draws the string `s` with its baseline starting at (`x`, `y`), in the
window's font and foreground colour; and so on for each group of three.
Produces `w`. Only the characters' shapes are drawn, not their
background.

**Errors:** 101 if a coordinate is not an integer; 103 if `s` is not a
string; 146 if a group is incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=60,30")
   DrawString(w, 5, 20, "Hi")
   n := 0
   every Pixel(w, 0, 0, 60, 30) ~== "65535,65535,65535" do n +:= 1
   write(n > 0 & "drawn")
end
```

Output:

```
drawn
```

**See also:** [TextWidth](#textwidth), [Font](#font).

## EraseArea

```
EraseArea(w, x, y, i1, i2, ...)
```

Fills the rectangle at (`x`, `y`), of width `i1` and height `i2`, with
the background colour; and so on for each group of four. Produces `w`.

**Defaults:** the whole window.

**Errors:** 101 if a coordinate is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=20,20", "fg=red")
   FillRectangle(w)
   EraseArea(w, 0, 0, 10, 10)
   write(Pixel(w, 5, 5, 1, 1), " ", Pixel(w, 15, 15, 1, 1))
end
```

Output:

```
65535,65535,65535 65535,0,0
```

**See also:** [Bg](#bg).

## Event

```
Event(w)
```

Produces the next event of `w`, waiting for one: a key's character as a
one-character string, or an integer, a key that has no character (an X
keysym) or a mouse event (`&lpress`, `&ldrag`, `&resize`, ...). Sets
`&x`, `&y`, `&row`, `&col`, `&interval`, and whether `&control`, `&meta`
and `&shift` succeed.

**Errors:** 143 if the event queue is malformed (as by [Pending](#pending)).

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 30, 40)
   write(Event(w), " ", &x, " ", &y)
end
```

Output:

```
-1 30 40
```

**See also:** [Pending](#pending), [&lpress](#lpress), [&x](#x).

## Fg

```
Fg(w, s)
```

Produces the foreground colour of `w`, after setting it to `s` if `s`
is given; fails if `s` is not a colour.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(Fg(w), " ", Fg(w, "red"), " ", Fg(w, "#00f"), " ", Fg(w, "no such colour") | "no")
end
```

Output:

```
black red #00f no
```

**See also:** [Bg](#bg), [ColorValue](#colorvalue).

## FillArc

```
FillArc(w, x, y, i1, i2, r1, r2, ...)
```

Fills the wedge of the ellipse that fits the rectangle at (`x`, `y`) of
width `i1` and height `i2`, from angle `r1` through `r2`; and so on for
each group of six. Produces `w`.

**Defaults:** `r1` 0; `r2` 2 pi: the whole ellipse.

**Errors:** 101 if a coordinate is not an integer; 102 if an angle is
not a number; 146 if a group is incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   FillArc(w, 10, 10, 20, 20)
   write(Pixel(w, 20, 20, 1, 1), " ", Pixel(w, 45, 45, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [DrawArc](#drawarc).

## FillCircle

```
FillCircle(w, x, y, r, r1, r2, ...)
```

Fills the circle (or the wedge of it from angle `r1` through `r2`) with
centre (`x`, `y`) and radius `r`; and so on for each group of five.
Produces `w`.

**Defaults:** `r1` 0; `r2` 2 pi.

**Errors:** 101 if a coordinate is not an integer; 102 if the radius or
an angle is not a number; 146 if a group is incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=50,50", "fg=red")
   FillCircle(w, 25, 25, 5)
   write(Pixel(w, 25, 25, 1, 1), " ", Pixel(w, 32, 25, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [DrawCircle](#drawcircle).

## FillPolygon

```
FillPolygon(w, x1, y1, x2, y2, ...)
```

Fills the polygon with the points as its corners, and produces `w`.

**Errors:** 101 if a coordinate is not an integer; 146 if a point is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=40,40", "fg=red")
   FillPolygon(w, 10, 10, 30, 10, 20, 30)
   write(Pixel(w, 20, 15, 1, 1), " ", Pixel(w, 11, 29, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [DrawPolygon](#drawpolygon).

## FillRectangle

```
FillRectangle(w, x, y, i1, i2, ...)
```

Fills the rectangle at (`x`, `y`), of width `i1` and height `i2`, with
the foreground colour (or the pattern, as `fillstyle` says); and so on
for each group of four. Produces `w`.

**Defaults:** the whole window.

**Errors:** 101 if a coordinate is not an integer; 146 if a group is
incomplete.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=30,30", "fg=red")
   FillRectangle(w, 10, 10, 5, 5)
   write(Pixel(w, 12, 12, 1, 1), " ", Pixel(w, 20, 20, 1, 1))
end
```

Output:

```
65535,0,0 65535,65535,65535
```

**See also:** [DrawRectangle](#drawrectangle), [Pattern](#pattern).

## Font

```
Font(w, s)
```

Produces the font of `w`, after setting it to `s` if `s` is given;
fails if there is no such font. A font is a family and options,
`"family,size"` or with `bold` and `italic`: the portable families are
`mono`, `typewriter`, `sans` and `serif`, and `fixed` is the default.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(Font(w), " ", Font(w, "mono,12"), " ", Font(w, "no such font") | "no")
end
```

Output:

```
fixed mono,12 no
```

**See also:** [TextWidth](#textwidth), [DrawString](#drawstring).

## FreeColor

```
FreeColor(w, s, ...)
```

Frees colours allocated by [NewColor](#newcolor), and produces `w`. Gcon
allocates none, so it frees nothing.

**Errors:** 103 if a colour is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(FreeColor(w, "red")))
end
```

Output:

```
window_1:1(demo)
```

## GotoRC

```
GotoRC(w, i1, i2)
```

Moves the text cursor of `w` to row `i1`, column `i2` (counting from 1,
in the font's character cells), where `write` to the window writes next,
and produces `w`.

**Defaults:** `i1`, `i2` 1.

**Errors:** 101 if `i1` or `i2` is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   GotoRC(w, 2, 3)
   write(WAttrib(w, "row"), " ", WAttrib(w, "col"))
end
```

Output:

```
2 3
```

**See also:** [GotoXY](#gotoxy).

## GotoXY

```
GotoXY(w, x, y)
```

Moves the text cursor of `w` to pixel (`x`, `y`), and produces `w`.

**Defaults:** `x`, `y` 0.

**Errors:** 101 if `x` or `y` is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   GotoXY(w, 5, 6)
   write(WAttrib(w, "x"), " ", WAttrib(w, "y"))
end
```

Output:

```
5 6
```

**See also:** [GotoRC](#gotorc).

## Lower

```
Lower(w)
```

Puts `w` behind the other windows, and produces it.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(Lower(w)))
end
```

Output:

```
window_1:1(demo)
```

**See also:** [Raise](#raise).

## NewColor

```
NewColor(w, s)
```

Allocates a mutable colour, set to `s`, and produces its number. Gcon
has no mutable colours, so `NewColor` fails.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(NewColor(w, "red") | "no mutable colours")
end
```

Output:

```
no mutable colours
```

**See also:** [Color](#color).

## PaletteChars

```
PaletteChars(w, s)
```

Produces the characters that stand for the colours of the palette `s`,
in order: `c1` to `c6` (colour palettes) or `g2` to `g256` (grey
scales); fails if there is no such palette.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(PaletteChars(w, "g16"))
end
```

Output:

```
0123456789ABCDEF
```

**See also:** [PaletteColor](#palettecolor), [DrawImage](#drawimage).

## PaletteColor

```
PaletteColor(w, s1, s2)
```

Produces the colour, `"r,g,b"`, that the character `s2` stands for in
the palette `s1`; fails if it stands for none.

**Errors:** 103 if `s1` or `s2` is not a string; 205 if `s2` is not one
character.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(PaletteColor(w, "g16", "0"), " ", PaletteColor(w, "g16", "F"))
end
```

Output:

```
0,0,0 65535,65535,65535
```

**See also:** [PaletteKey](#palettekey).

## PaletteKey

```
PaletteKey(w, s1, s2)
```

Produces the character of the palette `s1` whose colour is closest to
the colour `s2`; fails if `s2` is not a colour.

**Errors:** 103 if `s1` or `s2` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(PaletteKey(w, "g16", "white"), " ", PaletteKey(w, "g16", "black"))
end
```

Output:

```
F 0
```

**See also:** [PaletteColor](#palettecolor).

## Pattern

```
Pattern(w, s)
```

Sets the pattern that filling uses when `fillstyle` is `masked` or
`textured`, and produces `w`; fails if `s` is not a pattern. `s` is a
name (`black`, `verydark`, `darkgray`, `gray`, `lightgray`,
`verylight`, `white`, `vertical`, `diagonal`, `horizontal`, `grid`,
`trellis`, `checkers`, `grains`, `scales`, `waves`) or a bitmap,
`"width,#hex"`.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pattern(w, "checkers")
   write(WAttrib(w, "pattern"))
end
```

Output:

```
checkers
```

**See also:** [FillRectangle](#fillrectangle).

## Pending

```
Pending(w, x1, x2, ...)
```

Produces the list of events waiting for `w`, after adding `x1`, `x2`,
... to its end. An event takes three elements: the event, then x and y
(whose high bits carry the modifier keys and the interval).

**Errors:** 141 if `w` is not a window open for events.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(*Pending(w))
   Pending(w, "a", 1, 2)
   write(*Pending(w), " ", image(Event(w)))
end
```

Output:

```
0
3 "a"
```

**See also:** [Event](#event).

## Pixel

```
Pixel(w, x, y, i1, i2)
```

Generates the colours, `"r,g,b"`, of the pixels of the rectangle at
(`x`, `y`) of width `i1` and height `i2`, a row at a time from the top
left.

**Defaults:** the whole window.

**Errors:** 101 if a coordinate is not an integer.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=10,10", "fg=red")
   DrawPoint(w, 1, 0)
   every writes(Pixel(w, 0, 0, 2, 1), " ")
   write()
end
```

Output:

```
65535,65535,65535 65535,0,0 
```

## QueryPointer

```
QueryPointer(w)
```

Generates the mouse pointer's x and y, relative to `w` (or with no
window, to the screen).

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   every writes(type(QueryPointer(w)), " ")
   write()
end
```

Output:

```
integer integer 
```

## Raise

```
Raise(w)
```

Brings `w` in front of the other windows, and produces it.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(Raise(w)))
end
```

Output:

```
window_1:1(demo)
```

**See also:** [Lower](#lower).

## ReadImage

```
ReadImage(w, s, x, y)
```

Draws the image in the file `s` (GIF, PNG, JPEG, BMP or QOI) with its
top left at (`x`, `y`), and produces `&null`; fails if the file cannot
be read as an image.

**Defaults:** `x`, `y` 0.

**Errors:** 101 if `x` or `y` is not an integer; 103 if `s` is not a
string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=10,10", "fg=red")
   FillRectangle(w)
   WriteImage(w, "/tmp/gcon-doc-image.png")
   EraseArea(w)
   ReadImage(w, "/tmp/gcon-doc-image.png")
   write(Pixel(w, 5, 5, 1, 1))
   remove("/tmp/gcon-doc-image.png")
   write(ReadImage(w, "/no/such/file.gif") | "cannot read")
end
```

Output:

```
65535,0,0
cannot read
```

**See also:** [WriteImage](#writeimage).

## TextWidth

```
TextWidth(w, s)
```

Produces the width in pixels of `s` in the font of `w`.

**Errors:** 103 if `s` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(TextWidth(w, ""), " ", TextWidth(w, "ii") <= TextWidth(w, "iiii") & "wider")
end
```

Output:

```
0 wider
```

**See also:** [DrawString](#drawstring), [Font](#font).

## Uncouple

```
Uncouple(w)
```

Ends the window `w`'s binding of its canvas and context, and produces
`w`; the canvas closes when no window is left on it.

**Errors:** 140 if `w` is not a window.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   k := Couple(w, Clone(w))
   write(image(Uncouple(k)))
end
```

Output:

```
window_1:2(demo)
```

**See also:** [Couple](#couple).

## WAttrib

```
WAttrib(w, s1, s2, ...)
```

Sets or reads the attributes of `w`: each `s` is `"name=value"`, to
set, or `"name"`, to read; generates the value of each, in turn. Fails
for an attribute that cannot be set to that value. [Window attributes](#window-attributes)
lists them.

**Errors:** 109 if an attribute is not a string; 145 if it does not
exist.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=100,80")
   write(WAttrib(w, "size"), " ", WAttrib(w, "fg=green", "fg"))
   every writes(WAttrib(w, "width", "height"), " ")
   write()
end
```

Output:

```
100,80 green
100 80 
```

## WDefault

```
WDefault(w, s1, s2)
```

Produces the value of the option `s2` of the program `s1` in the
window system's defaults (X resources); there are none here, so
`WDefault` fails.

**Errors:** 103 if `s1` or `s2` is not a string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(WDefault(w, "prog", "font") | "no default")
end
```

Output:

```
no default
```

## WFlush

```
WFlush(w)
```

Shows everything drawn on `w` so far, and produces `w`.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(WFlush(w)))
end
```

Output:

```
window_1:1(demo)
```

**See also:** [WSync](#wsync).

## WriteImage

```
WriteImage(w, s, x, y, i1, i2)
```

Writes the rectangle at (`x`, `y`), of width `i1` and height `i2`, to
the file `s`, in the format its name's extension says (`.png`, `.jpg`,
`.bmp`, `.qoi`; GIF otherwise), and produces `w`; fails if it cannot.

**Defaults:** the whole window.

**Errors:** 101 if a coordinate is not an integer; 103 if `s` is not a
string.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "size=10,10")
   write(image(WriteImage(w, "/tmp/gcon-doc-write.gif")))
   remove("/tmp/gcon-doc-write.gif")
end
```

Output:

```
window_1:1(demo)
```

**See also:** [ReadImage](#readimage).

## WSync

```
WSync(w)
```

Waits until the display server has carried out everything asked of it
for `w`, and produces `w`.

**Errors:** 140 if `w` is not a window.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   write(image(WSync(w)))
end
```

Output:

```
window_1:1(demo)
```

**See also:** [WFlush](#wflush).

## &col

```
&col
```

The text column of the last event's x (in the font's character cells,
from 1). Assigning to it sets `&x` to match.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "font=mono,12")
   Pending(w, &lpress, 0, 0)
   Event(w)
   write(&col)
end
```

Output:

```
1
```

**See also:** [&x](#x), [&row](#row).

## &control

```
&control
```

Succeeds, producing `&null`, if the control key was held at the last
event; fails otherwise.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 65536 + 1, 1)
   Event(w)
   write(&control & "control held")
end
```

Output:

```
control held
```

**See also:** [&meta](#meta), [&shift](#shift).

## &interval

```
&interval
```

The time in milliseconds between the last event and the one before.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 1, 1 + 3 * 65536)
   Event(w)
   write(&interval)
end
```

Output:

```
3
```

## &ldrag

```
&ldrag
```

The event of moving the mouse with its left button down: -7.

```icon
procedure main()
   write(&ldrag)
end
```

Output:

```
-7
```

**See also:** [&lpress](#lpress), [Event](#event).

## &lpress

```
&lpress
```

The event of pressing the left mouse button: -1.

```icon
procedure main()
   write(&lpress)
end
```

Output:

```
-1
```

**See also:** [&lrelease](#lrelease), [Event](#event).

## &lrelease

```
&lrelease
```

The event of releasing the left mouse button: -4.

```icon
procedure main()
   write(&lrelease)
end
```

Output:

```
-4
```

## &mdrag

```
&mdrag
```

The event of moving the mouse with its middle button down: -8.

```icon
procedure main()
   write(&mdrag)
end
```

Output:

```
-8
```

## &meta

```
&meta
```

Succeeds, producing `&null`, if the meta key was held at the last event
(Option on macOS, or Command with `ICONSDL_META=command`); fails
otherwise.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 1, 1)
   Event(w)
   write((&meta & "meta held") | "no meta")
end
```

Output:

```
no meta
```

**See also:** [&control](#control), [&shift](#shift).

## &mpress

```
&mpress
```

The event of pressing the middle mouse button: -2.

```icon
procedure main()
   write(&mpress)
end
```

Output:

```
-2
```

## &mrelease

```
&mrelease
```

The event of releasing the middle mouse button: -5.

```icon
procedure main()
   write(&mrelease)
end
```

Output:

```
-5
```

## &rdrag

```
&rdrag
```

The event of moving the mouse with its right button down: -9.

```icon
procedure main()
   write(&rdrag)
end
```

Output:

```
-9
```

## &resize

```
&resize
```

The event of the window's being resized: -10.

```icon
procedure main()
   write(&resize)
end
```

Output:

```
-10
```

## &row

```
&row
```

The text row of the last event's y, counted in the font's lines.
Assigning to it sets `&y` to match.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden", "font=mono,12")
   Pending(w, &lpress, 30, 30)
   Event(w)
   write(&row)
end
```

Output:

```
3
```

**See also:** [&y](#y), [&col](#col).

## &rpress

```
&rpress
```

The event of pressing the right mouse button: -3.

```icon
procedure main()
   write(&rpress)
end
```

Output:

```
-3
```

## &rrelease

```
&rrelease
```

The event of releasing the right mouse button: -6.

```icon
procedure main()
   write(&rrelease)
end
```

Output:

```
-6
```

## &shift

```
&shift
```

Succeeds, producing `&null`, if the shift key was held at the last
event; fails otherwise.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 1, 1)
   Event(w)
   write((&shift & "shift held") | "no shift")
end
```

Output:

```
no shift
```

**See also:** [&control](#control), [&meta](#meta).

## &window

```
&window
```

The window the graphics functions use when none is given; `&null` at
first. `open` does not set it; assign to it, or use the IPL's `WOpen`.

```icon
procedure main()
   write(image(&window))
   &window := open("demo", "g", "canvas=hidden")
   write(image(Fg()))
end
```

Output:

```
&null
"black"
```

## &x

```
&x
```

The x coordinate of the last event. Assigning to it sets `&col` to
match.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, &lpress, 30, 40)
   Event(w)
   write(&x, " ", &y)
end
```

Output:

```
30 40
```

**See also:** [&y](#y), [&col](#col).

## &y

```
&y
```

The y coordinate of the last event. Assigning to it sets `&row` to
match.

```icon
procedure main()
   w := open("demo", "g", "canvas=hidden")
   Pending(w, "a", 5, 7)
   Event(w)
   write(&y)
end
```

Output:

```
7
```

**See also:** [&x](#x), [&row](#row).
