# Gcon's standard library

Gcon will have a standard library of its own: a small, curated set of
procedures, documented in this tree as the built-in functions are, and
kept working with each release. It is still to be decided what belongs
in it.

Until then, Gcon ships [the Icon Program Library](ipl/index.md) as
Arizona wrote it, so that Icon programs that `link` it run unchanged.
Its files marked as candidates are the first list for the standard
library: those that Gcon's own programs (vib and the graphics programs
in `bin`) need.

One module is Gcon's own already: [uclass](ref/uclass.md), the Unicode
character classes as usets, in `lib/gcon`, which is searched after the
IPL ([where linked files are found](help/library.md)).
