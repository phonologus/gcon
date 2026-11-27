/*
 * The documentation's renderers (mdrender.h). Lists are opened and
 * closed as the blocks' depths change, with a stack, not recursion.
 */
#include "mdrender.h"

#include <stdio.h>
#include <string.h>

bool md_external(const char *t) { return strstr(t, "://") || !strncmp(t, "mailto:", 7); }

/* ------------------------------------------------------------------ */
/* HTML */

void md_htmlesc(Buf *out, const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		switch (s[i]) {
		case '&': bufputs(out, "&amp;"); break;
		case '<': bufputs(out, "&lt;"); break;
		case '>': bufputs(out, "&gt;"); break;
		case '"': bufputs(out, "&quot;"); break;
		default: bufputc(out, s[i]);
		}
	}
}

static void esc(Buf *out, const char *s) { md_htmlesc(out, s, strlen(s)); }

/* htmllink appends the href for a link target. */
static void htmllink(Buf *out, const char *t) {
	if (md_external(t) || t[0] == '#') {
		esc(out, t);
		return;
	}
	const char *hash = strchr(t, '#');
	size_t pn = hash ? (size_t)(hash - t) : strlen(t);
	if (pn >= 3 && !strncmp(t + pn - 3, ".md", 3)) {
		md_htmlesc(out, t, pn - 3);
		bufputs(out, ".html");
		if (hash)
			esc(out, hash);
	} else
		esc(out, t);
}

static void htmlspans(Buf *out, const MdSpans *s) {
	for (size_t i = 0; i < s->n; i++) {
		const MdSpan *sp = &s->v[i];
		switch (sp->kind) {
		case MS_TEXT: esc(out, sp->s); break;
		case MS_CODE:
			bufputs(out, "<code>");
			esc(out, sp->s);
			bufputs(out, "</code>");
			break;
		case MS_EM: bufputs(out, sp->end ? "</em>" : "<em>"); break;
		case MS_STRONG: bufputs(out, sp->end ? "</strong>" : "<strong>"); break;
		case MS_LINK:
			if (sp->end)
				bufputs(out, "</a>");
			else {
				bufputs(out, "<a href=\"");
				htmllink(out, sp->s);
				bufputs(out, "\">");
			}
			break;
		}
	}
}

typedef struct OpenList {
	bool ordered;
	bool item; /* an <li> is open */
} OpenList;

void md_html(Buf *out, const MdDoc *d) {
	OpenList lists[3];
	int n = 0;
	for (size_t bi = 0; bi < d->blocks.n; bi++) {
		const MdBlock *b = &d->blocks.v[bi];
		/* the lists this block is outside of */
		int keep = b->kind == MB_ITEM ? b->level + 1 : b->depth;
		if (b->kind == MB_ITEM && keep <= n && lists[keep - 1].ordered != b->ordered)
			keep--; /* another kind of list at this level */
		while (n > keep) {
			n--;
			if (lists[n].item)
				bufputs(out, "</li>\n");
			bufputs(out, lists[n].ordered ? "</ol>\n" : "</ul>\n");
		}
		if (b->kind == MB_ITEM) {
			if (n == b->level + 1) {
				if (lists[n - 1].item)
					bufputs(out, "</li>\n");
			} else {
				lists[n].ordered = b->ordered;
				if (b->ordered && b->number != 1)
					bufprintf(out, "<ol start=\"%d\">\n", b->number);
				else
					bufputs(out, b->ordered ? "<ol>\n" : "<ul>\n");
				n++;
			}
			lists[n - 1].item = true;
			bufputs(out, "<li>");
			htmlspans(out, &b->spans);
			bufputc(out, '\n');
			continue;
		}
		switch (b->kind) {
		case MB_HEADING:
			bufprintf(out, "<h%d id=\"", b->level);
			esc(out, b->anchor);
			bufputs(out, "\">");
			htmlspans(out, &b->spans);
			bufprintf(out, "</h%d>\n", b->level);
			break;
		case MB_PARA:
			bufputs(out, "<p>");
			htmlspans(out, &b->spans);
			bufputs(out, "</p>\n");
			break;
		case MB_CODE:
			bufputs(out, "<pre><code");
			if (b->lang && *b->lang) {
				bufputs(out, " class=\"language-");
				esc(out, b->lang);
				bufputs(out, "\"");
			}
			bufputs(out, ">");
			esc(out, b->code);
			bufputs(out, "</code></pre>\n");
			break;
		case MB_TABLE:
			bufputs(out, "<table>\n<thead>\n");
			for (int r = 0; r < b->nrows; r++) {
				if (r == 1)
					bufputs(out, "</thead>\n<tbody>\n");
				bufputs(out, "<tr>");
				for (int c = 0; c < b->ncols; c++) {
					bufputs(out, r ? "<td>" : "<th>");
					htmlspans(out, &b->cells[r * b->ncols + c]);
					bufputs(out, r ? "</td>" : "</th>");
				}
				bufputs(out, "</tr>\n");
			}
			bufputs(out, b->nrows > 1 ? "</tbody>\n</table>\n" : "</thead>\n</table>\n");
			break;
		case MB_ITEM: break;
		}
	}
	while (n > 0) {
		n--;
		if (lists[n].item)
			bufputs(out, "</li>\n");
		bufputs(out, lists[n].ordered ? "</ol>\n" : "</ul>\n");
	}
}

/* ------------------------------------------------------------------ */
/* Text */

/* textspans appends the spans as plain text: markup dropped, an
 * external link's target after its text. */
static void textspans(Buf *out, const MdSpans *s) {
	const char *target = NULL;
	for (size_t i = 0; i < s->n; i++) {
		const MdSpan *sp = &s->v[i];
		switch (sp->kind) {
		case MS_TEXT:
		case MS_CODE: bufputs(out, sp->s); break;
		case MS_EM:
		case MS_STRONG: break;
		case MS_LINK:
			if (!sp->end)
				target = sp->s;
			else if (target && md_external(target))
				bufprintf(out, " (%s)", target);
			break;
		}
	}
}

/* width is the number of characters in s (n bytes) of UTF-8. */
static int width(const char *s, size_t n) {
	int w = 0;
	for (size_t i = 0; i < n; i++)
		if (((unsigned char)s[i] & 0xC0) != 0x80)
			w++;
	return w;
}

static void spaces(Buf *out, int n) {
	for (int i = 0; i < n; i++)
		bufputc(out, ' ');
}

/* fill appends text filled to cols columns, the first line starting at
 * column first (already written) and the others indented by indent. */
static void fill(Buf *out, const char *text, int first, int indent, int cols) {
	int col = first;
	bool linestart = true;
	const char *s = text;
	while (*s) {
		while (*s == ' ')
			s++;
		if (!*s)
			break;
		const char *e = s;
		while (*e && *e != ' ')
			e++;
		int w = width(s, (size_t)(e - s));
		if (!linestart && col + 1 + w > cols) {
			bufputc(out, '\n');
			spaces(out, indent);
			col = indent;
			linestart = true;
		}
		if (!linestart) {
			bufputc(out, ' ');
			col++;
		}
		bufadd(out, s, (size_t)(e - s));
		col += w;
		linestart = false;
		s = e;
	}
	bufputc(out, '\n');
}

/* table appends a table as aligned columns, indented. */
static void table(Buf *out, const MdBlock *b, int indent, const char *linestart) {
	int *w = xcalloc((size_t)b->ncols, sizeof *w);
	char **cell = xcalloc((size_t)(b->nrows * b->ncols), sizeof *cell);
	for (int i = 0; i < b->nrows * b->ncols; i++) {
		Buf t = {0};
		textspans(&t, &b->cells[i]);
		cell[i] = t.b ? t.b : "";
		int cw = width(cell[i], strlen(cell[i]));
		if (cw > w[i % b->ncols])
			w[i % b->ncols] = cw;
	}
	for (int r = 0; r < b->nrows; r++) {
		for (int pass = 0; pass < (r == 0 ? 2 : 1); pass++) {
			bufputs(out, linestart);
			spaces(out, indent);
			Buf line = {0};
			for (int c = 0; c < b->ncols; c++) {
				const char *t = cell[r * b->ncols + c];
				int tw = pass ? w[c] : width(t, strlen(t));
				if (pass)
					for (int k = 0; k < w[c]; k++)
						bufputc(&line, '-');
				else
					bufputs(&line, t);
				if (c + 1 < b->ncols)
					spaces(&line, w[c] - tw + 2);
			}
			while (line.n && line.b[line.n - 1] == ' ')
				line.b[--line.n] = 0;
			if (line.b)
				bufputs(out, line.b);
			bufputc(out, '\n');
		}
	}
}

void md_text(Buf *out, const MdDoc *d, int cols) {
	const MdBlock *prev = NULL;
	for (size_t bi = 0; bi < d->blocks.n; bi++) {
		const MdBlock *b = &d->blocks.v[bi];
		bool samelist = prev && b->kind == MB_ITEM && prev->kind == MB_ITEM &&
				(b->level != prev->level || b->ordered == prev->ordered);
		if (prev && !samelist)
			bufputc(out, '\n');
		prev = b;
		int indent = 4 * b->depth;
		Buf t = {0};
		switch (b->kind) {
		case MB_HEADING:
			textspans(&t, &b->spans);
			bufputs(out, t.b ? t.b : "");
			bufputc(out, '\n');
			if (b->level <= 2) {
				int w = width(t.b ? t.b : "", t.n);
				for (int k = 0; k < w; k++)
					bufputc(out, b->level == 1 ? '=' : '-');
				bufputc(out, '\n');
			}
			break;
		case MB_PARA:
			textspans(&t, &b->spans);
			spaces(out, indent);
			fill(out, t.b ? t.b : "", indent, indent, cols);
			break;
		case MB_ITEM: {
			int text = 4 * (b->level + 1);
			char marker[16];
			if (b->ordered)
				snprintf(marker, sizeof marker, "%d. ", b->number);
			else
				snprintf(marker, sizeof marker, "- ");
			int mw = (int)strlen(marker);
			spaces(out, text - mw > 0 ? text - mw : 0);
			bufputs(out, marker);
			textspans(&t, &b->spans);
			fill(out, t.b ? t.b : "", text > mw ? text : mw, text, cols);
			break;
		}
		case MB_CODE:
			for (const char *s = b->code; *s;) {
				const char *e = strchr(s, '\n');
				size_t n = e ? (size_t)(e - s) : strlen(s);
				if (n) {
					spaces(out, indent + 4);
					bufadd(out, s, n);
				}
				bufputc(out, '\n');
				s += n + (e ? 1 : 0);
			}
			break;
		case MB_TABLE: table(out, b, indent, ""); break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* roff */

/* roffesc appends s for roff: a backslash as \e, and in code a hyphen
 * as \-, which man pages show as the ASCII hyphen-minus. */
static void roffesc(Buf *out, const char *s, bool code) {
	for (; *s; s++) {
		if (*s == '\\')
			bufputs(out, "\\e");
		else if (*s == '-' && code)
			bufputs(out, "\\-");
		else
			bufputc(out, *s);
	}
}

static void roffspans(Buf *out, const MdSpans *s) {
	const char *target = NULL;
	for (size_t i = 0; i < s->n; i++) {
		const MdSpan *sp = &s->v[i];
		switch (sp->kind) {
		case MS_TEXT: roffesc(out, sp->s, false); break;
		case MS_CODE:
			bufputs(out, "\\fB");
			roffesc(out, sp->s, true);
			bufputs(out, "\\fR");
			break;
		case MS_EM: bufputs(out, sp->end ? "\\fR" : "\\fI"); break;
		case MS_STRONG: bufputs(out, sp->end ? "\\fR" : "\\fB"); break;
		case MS_LINK:
			if (!sp->end)
				target = sp->s;
			else if (target && md_external(target)) {
				bufputs(out, " <");
				roffesc(out, target, true);
				bufputs(out, ">");
			}
			break;
		}
	}
}

/* roffline appends a line of text, guarded so that it cannot start a
 * request. */
static void roffline(Buf *out, const MdSpans *s) {
	bufputs(out, "\\&");
	roffspans(out, s);
	bufputc(out, '\n');
}

static void upper(Buf *out, const char *s) {
	for (; *s; s++)
		bufputc(out, *s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s);
}

/* paragraph starts a paragraph at depth: .PP at the top (unless a
 * heading has just begun one), else indented as an item's text. */
static void paragraph(Buf *out, int depth, bool fresh) {
	if (depth)
		bufputs(out, ".IP \"\" 4\n");
	else if (!fresh)
		bufputs(out, ".PP\n");
}

void md_roff(Buf *out, const MdDoc *d, const char *name, const char *footer) {
	bufputs(out, ".TH \"");
	upper(out, name);
	bufputs(out, "\" 7 \"");
	roffesc(out, footer, false);
	bufputs(out, "\" \"Gcon\" \"Gcon documentation\"\n");
	int rs = 0; /* .RS levels open */
	bool titled = false;
	for (size_t bi = 0; bi < d->blocks.n; bi++) {
		const MdBlock *b = &d->blocks.v[bi];
		/* just after .SH or .SS, where .PP is implied */
		bool fresh = bi > 0 && d->blocks.v[bi - 1].kind == MB_HEADING;
		int want = b->kind == MB_ITEM ? b->level : b->depth;
		if (b->kind != MB_ITEM && b->depth > 0)
			want = b->depth - 1; /* inside an item: its .IP indent */
		while (rs > want) {
			bufputs(out, ".RE\n");
			rs--;
		}
		while (rs < want) {
			bufputs(out, ".RS 4\n");
			rs++;
		}
		Buf t = {0};
		switch (b->kind) {
		case MB_HEADING:
			md_plain(&t, &b->spans);
			if (b->level == 1 && !titled) {
				titled = true;
				bufputs(out, ".SH NAME\n");
				roffline(out, &b->spans);
			} else if (b->level <= 2) {
				bufputs(out, ".SH \"");
				Buf u = {0};
				roffspans(&u, &b->spans);
				upper(out, u.b ? u.b : "");
				bufputs(out, "\"\n");
			} else {
				bufputs(out, ".SS \"");
				roffspans(out, &b->spans);
				bufputs(out, "\"\n");
			}
			break;
		case MB_PARA:
			paragraph(out, b->depth, fresh);
			roffline(out, &b->spans);
			break;
		case MB_ITEM:
			if (b->ordered)
				bufprintf(out, ".IP \"%d.\" 4\n", b->number);
			else
				bufputs(out, ".IP \\(bu 4\n");
			roffline(out, &b->spans);
			break;
		case MB_CODE:
			paragraph(out, b->depth, fresh);
			bufputs(out, ".RS 4\n.nf\n");
			for (const char *s = b->code; *s;) {
				const char *e = strchr(s, '\n');
				size_t n = e ? (size_t)(e - s) : strlen(s);
				char *ln = xstrndup(s, n);
				bufputs(out, "\\&");
				roffesc(out, ln, true);
				bufputc(out, '\n');
				s += n + (e ? 1 : 0);
			}
			bufputs(out, ".fi\n.RE\n");
			break;
		case MB_TABLE: {
			paragraph(out, b->depth, fresh);
			bufputs(out, ".RS 4\n.nf\n");
			Buf tb = {0};
			table(&tb, b, 0, "");
			for (const char *s = tb.b ? tb.b : ""; *s;) {
				const char *e = strchr(s, '\n');
				size_t n = e ? (size_t)(e - s) : strlen(s);
				bufputs(out, "\\&");
				roffesc(out, xstrndup(s, n), false);
				bufputc(out, '\n');
				s += n + (e ? 1 : 0);
			}
			bufputs(out, ".fi\n.RE\n");
			break;
		}
		}
	}
	while (rs-- > 0)
		bufputs(out, ".RE\n");
}
