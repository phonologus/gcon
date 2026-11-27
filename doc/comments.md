# Doc comments

A doc comment is the comment directly above a procedure, record or
global declaration, with no blank line between them. `gcon doc
prog.icn` shows a file's doc comments, as `go doc` shows a package's;
`gcon doc prog.icn name` shows one declaration's; and the language
server shows a declaration's doc comment when you hover over its name.
The comment at the top of a file, separated from the first declaration
by a blank line (or followed by code that is not a declaration), is the
file's own.

```icon
# total adds up the numbers in L, which may be integers or reals.
#
# It fails for an empty list:
#
#     total([])   # fails
procedure total(L)
   local t
   every (/t := 0) +:= !L
   return \t
end
```

As in Go, doc comments are plain text, read by a few rules:

- A blank comment line separates paragraphs. Lines of a paragraph are
  joined, so write them as you would prose.
- Lines indented more than the rest of the comment are kept as they
  are, as code; blank lines between them stay in the code.
- A line beginning `-`, `*`, `+` or a number and `.` is a list item;
  lines indented under it continue it.
- A line `# Heading` (written `## Heading` in the source, after the
  comment's own `#`) is a heading.
- A rule, a line of `#`, `=`, `-` or `*`, is dropped, so the boxed
  headers of the Icon Program Library read cleanly.
- Everything else is text: stars, underscores and backquotes are shown
  as they are written.

Begin with a sentence that says what the declaration does, starting
with its name ("total adds up..."): it is the summary. The IPL's
convention, a `#:` comment on the procedure's own line
(`procedure total(L) #: sum of a list`), is read as the doc comment of
a procedure that has none above it.
