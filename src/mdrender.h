/*
 * The documentation's renderers (gcon doc): a parsed page (markdown.h)
 * as HTML, plain text or roff. Each shows the whole subset.
 */
#ifndef GCON_MDRENDER_H
#define GCON_MDRENDER_H

#include "markdown.h"

/* md_html appends the page's body as HTML. A relative link to a page,
 * "x.md" or "x.md#anchor", becomes "x.html" with the anchor. */
void md_html(Buf *out, const MdDoc *d);

/* md_text appends the page as plain text, paragraphs filled to width
 * columns. An external link's target follows its text. */
void md_text(Buf *out, const MdDoc *d, int width);

/* md_roff appends the page as roff, with man(7)'s macros: name is the
 * page's name for .TH, footer its centre footer (Gcon's version). */
void md_roff(Buf *out, const MdDoc *d, const char *name, const char *footer);

/* md_htmlesc appends s with &, <, > and " escaped. */
void md_htmlesc(Buf *out, const char *s, size_t n);

/* md_external reports whether a link target leaves the tree (a URL). */
bool md_external(const char *target);

#endif
