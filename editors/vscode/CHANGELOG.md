# Changelog

## 0.4.1

With Gcon v0.4.1, whose `gcon doc` serves the documentation, with
runnable examples (no change to the extension itself).

## 0.4.0

With Gcon v0.4.0, which documents itself (`gcon help`, `gcon doc`). The
extension is "Icon (Gcon)" now. Hover shows Gcon's own reference for
the built-in functions and keywords and for `uclass`, in place of the fetched
Arizona pages (still the fallback where there is no entry), and shows doc
comments formatted by Go's rules (paragraphs, code, lists). The language
server finds linked files in the workspace folders too, and warns of doc
comments that will not read as meant.

## 0.3.1

With gcon v0.3.1, which is faster (no change to the extension itself).

## 0.3.0

With gcon v0.3.0: highlights uset literals (`u'...'`) and the new built-ins
`uset`, `uord` and `uvalid` (`utf8valid` renamed); the built-ins `len`,
`readrunes`, `runes` and `bytes` are gone. The language server documents
the additions, reports uset literal errors, follows `link` into gcon's own
library (`lib/gcon`), and warns of a cset and a uset mixed and of a link
to the program's own file.

## 0.2.0

With gcon v0.2.0: highlights gcon's new built-in `readrunes`, and the
language server documents it and `reads`, which now reads bytes.

## 0.1.0

First release, forked from icontools' Icon extension: highlighting with
gcon's additions, and gcon's language server (diagnostics and vet
warnings, hover, definition, references, completion, signature help,
rename, formatting).
