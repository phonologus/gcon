# Changelog

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
