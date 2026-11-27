/*
 * The documentation's Markdown (gcon doc): a small subset, read
 * strictly, so that every reader (HTML, text, roff) can show all of it
 * and the renderers stay small. Anything outside the subset is an
 * error, reported at its line. The subset (design/development.md,
 * "Documentation"):
 *
 *   blocks:  # to #### headings; paragraphs; "- " and "1. " list items,
 *            nested one level (by indenting to the item's text); fenced
 *            code (```, with an optional language); tables (a header
 *            row, a |---| row, then rows, all with the same cells)
 *   inline:  `code`, *emphasis*, **strong**, [text](target), and a
 *            backslash before punctuation for the character itself
 *
 * Not in it: block quotes, HTML, images, indented code, rules, setext
 * headings, "*" or "+" items, reference links, "_" emphasis, tabs
 * outside code.
 *
 * Each heading has an anchor, as GitHub makes them: the text in lower
 * case, spaces as "-", other punctuation dropped, "-1", "-2" for
 * repeats.
 */
#ifndef GCON_MARKDOWN_H
#define GCON_MARKDOWN_H

#include "util.h"

/* An inline span. */
typedef enum MdSpanKind {
	MS_TEXT,   /* text: s */
	MS_CODE,   /* code: s */
	MS_EM,     /* *emphasis* begins or (end) ends */
	MS_STRONG, /* **strong** begins or ends */
	MS_LINK,   /* a link begins (target: s) or ends */
} MdSpanKind;

typedef struct MdSpan {
	MdSpanKind kind;
	bool end;
	char *s;
} MdSpan;

typedef VEC(MdSpan) MdSpans;

typedef enum MdBlockKind {
	MB_HEADING, /* level 1-4; spans; anchor */
	MB_PARA,    /* spans */
	MB_ITEM,    /* a list item: list level (0 or 1), ordered, number; spans: its first paragraph */
	MB_CODE,    /* code (the lines, each ending "\n"), lang (may be "") */
	MB_TABLE,   /* cells: rows of ncols spans, the first row the header */
} MdBlockKind;

typedef struct MdBlock {
	MdBlockKind kind;
	int line;   /* where it starts */
	int depth;  /* 0 at the top; d+1 inside an item of list level d */
	int level;  /* a heading's level, an item's list level */
	bool ordered;
	int number;
	MdSpans spans;
	char *anchor;
	char *code, *lang;
	int nrows, ncols;
	MdSpans *cells; /* nrows * ncols */
} MdBlock;

typedef struct MdDoc {
	VEC(MdBlock) blocks;
	char *title; /* the first heading's plain text, or "" */
	VEC(char *) errs; /* "line N: message" */
} MdDoc;

/* md_parse reads src (n bytes). Errors are collected in doc->errs; the
 * blocks read are kept either way. */
MdDoc *md_parse(const char *src, size_t n);

/* md_plain appends the spans' text without markup. */
void md_plain(Buf *b, const MdSpans *s);

/* md_slug is the anchor GitHub makes of a heading's plain text (without
 * the repeat suffix). */
char *md_slug(const char *text);

#endif
