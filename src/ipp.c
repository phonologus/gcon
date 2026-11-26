/*
 * ipp.c -- the Icon preprocessor (ipp.h).
 *
 * This is icont's preprocessor (icon/src/common/ipp.c, public domain),
 * modified for gcon:
 *
 *  - input is read from memory (the main file) or whole files (those
 *    $included), not a line at a time from stdio;
 *  - output is a buffer with one line for each line read (directive and
 *    skipped lines empty) and a map of where each came from, instead of
 *    a character stream with #line markers (pushline is gone);
 *  - errors are collected, not printed;
 *  - the nested $if skip (skipcode) counts its depth instead of
 *    recursing, so nesting has no ceiling (docs/design.md, "Two
 *    oracles");
 *  - character classes are ASCII's, whatever the locale or signedness
 *    of char; and in ordinary lines a NUL byte is an ordinary character,
 *    where icont's line reader would cut the line short (directive
 *    lines still end at one).
 *
 * The scanners (wskip, nskip, matchq, getidt, getfnm), the definition
 * table (dlookup), the directives and their messages, and the expansion
 * loop (ppch's) are icont's.
 */
#include "ipp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTBINS 256 /* number of hash bins */

typedef struct infile { /* input file structure */
	struct infile *prev; /* previous file */
	char *fname;         /* file name */
	long lno;            /* line number */
	const char *data;    /* its contents */
	size_t n, at;        /* their length, the next to read */
	int ifdepth;         /* $if nesting depth when opened */
} infile;

typedef struct cdefn { /* structure holding a definition */
	struct cdefn *next;  /* link to next defn */
	size_t nlen, vlen;   /* length of name & val */
	bool inuse;          /* set if currently being expanded */
	char *s;             /* name then value, then \0 */
} cdefn;

typedef struct buffer { /* a pending buffer: the place in the one below */
	cdefn *defn;            /* definition being processed */
	const char *ptr, *stop, *lim;
} buffer;

typedef struct PP {
	const PPOptions *opts;
	PPResult *r;
	infile *curfile;
	cdefn *cbin[HTBINS];
	int ifdepth;
	Buf line; /* the line read, ending "\n\0" */
	VEC(buffer) bstack;
} PP;

/* ASCII character classes, as icont's ctype calls in the C locale */
static bool isalpha_(int c) { return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'); }
static bool isdigit_(int c) { return '0' <= c && c <= '9'; }
static bool isalnum_(int c) { return isalpha_(c) || isdigit_(c); }
static bool isspace_(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static bool ispunct_(int c) { return c > ' ' && c < 0x7f && !isalnum_(c); }
static bool isidch(int c) { return isalnum_(c) || c == '_'; }

/* ------------------------------------------------------------------ */
/* Errors and output */

/*
 * pfatal(s1,s2) -- record a preprocessing error.
 *
 *  s1 is the error message; s2 is the offending value, if any.
 *  If s2 ends in a newline, the newline is not shown.
 */
static void pfatal(PP *pp, const char *s1, const char *s2) {
	Buf b = {0};
	if (s2 && *s2) {
		size_t n = strlen(s2);
		if (s2[n - 1] == '\n')
			n--;
		bufputc(&b, '"');
		bufadd(&b, s2, n);
		bufputs(&b, "\": ");
	}
	bufputs(&b, s1);
	PPError e = {pp->curfile->fname, pp->curfile->lno, b.b};
	VPUSH(pp->r->errs, e);
}

char *ipp_error(const PPError *e) {
	Buf b = {0};
	bufprintf(&b, "File %s; Line %ld # %s", e->file, e->line, e->msg);
	return b.b;
}

/* mark starts an output line from line lno of f. */
static void mark(PP *pp, infile *f, long lno) {
	PPPos pos = {f->fname, lno};
	VPUSH(pp->r->map.starts, pp->r->src.n);
	VPUSH(pp->r->map.pos, pos);
}

static void emptyline(PP *pp, infile *f, long lno) {
	mark(pp, f, lno);
	bufputc(&pp->r->src, '\n');
}

void ipp_locate(const PPMap *m, size_t off, const char **file, long *line) {
	size_t lo = 0, hi = m->starts.n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (m->starts.v[mid] <= off)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0) {
		*file = m->pos.n ? m->pos.v[0].file : "";
		*line = m->pos.n ? m->pos.v[0].line : 1;
		return;
	}
	*file = m->pos.v[lo - 1].file;
	*line = m->pos.v[lo - 1].line;
}

/* ------------------------------------------------------------------ */
/* Token scanning functions (on NUL-terminated text) */

/*
 * wskip(s) -- skip whitespace and return updated pointer
 *
 *  If '#' is encountered, skips to end of string.
 */
static char *wskip(char *s) {
	char c;
	while (isspace_(c = *s))
		s++;
	if (c == '#')
		while ((c = *++s) != 0)
			;
	return s;
}

/*
 * nskip(s, lim) -- skip over numeric constant and return updated
 *  pointer, going no further than lim.
 */
static const char *nskip(const char *s, const char *lim) {
#define NEXT (++s < lim ? *s : 0)
	char c;
	while (isdigit_(c = NEXT))
		;
	if (c == 'r' || c == 'R') {
		while (isalnum_(c = NEXT))
			;
		return s;
	}
	if (c == '.')
		while (isdigit_(c = NEXT))
			;
	if (c == 'e' || c == 'E') {
		c = s + 1 < lim ? s[1] : 0;
		if (c == '+' || c == '-')
			s++;
		while (isdigit_(c = NEXT))
			;
	}
	return s;
#undef NEXT
}

/*
 * matchq(s, lim) -- scan for matching quote character and return
 *  pointer.
 *
 *  Taking *s as the quote character, s is incremented until it points
 *  to either another occurrence of the character or lim. Escaped quote
 *  characters do not stop the scan. The updated pointer is returned.
 */
static const char *matchq(const char *s, const char *lim) {
	char c, q;
	if (s >= lim)
		return s;
	q = *s;
	while (++s < lim && (c = *s) != q) {
		if (c == '\\')
			if (++s >= lim)
				return lim;
	}
	return s;
}

/* matchqz is matchq on NUL-terminated text, as icont's. */
static char *matchqz(char *s) { return (char *)matchq(s, s + strlen(s)); }

/*
 * getidt(dst,src) -- extract identifier, return updated pointer
 *
 *  The identifier (in Icon terms, "many(&letters++&digits++'_')")
 *  at src is copied to dst and '\0' is appended.  A pointer to the
 *  character following the identifier is returned.
 *
 *  dst may partially overlap src if dst has a lower address.  This
 *  is typically done to avoid the need for another arbitrarily-long
 *  buffer.  An offset of -1 allows room for insertion of the '\0'.
 */
static char *getidt(char *dst, char *src) {
	char c;
	while (isidch(c = *src)) {
		*dst++ = c;
		src++;
	}
	*dst = '\0';
	return src;
}

/*
 * getfnm(dst,src) -- extract filename, return updated pointer
 *
 *  Similarly to getidt, getfnm extracts a quoted or unquoted file name.
 *  An empty string at dst indicates a missing or unterminated file name.
 */
static char *getfnm(char *dst, char *src) {
	char *lim;
	if (*src != '"')
		return getidt(dst, src);
	lim = matchqz(src);
	if (*lim != '"') {
		*dst = '\0';
		return lim;
	}
	while (++src < lim)
		if ((*dst++ = *src) == '\\')
			dst[-1] = *++src;
	*dst = '\0';
	return lim + 1;
}

/* ------------------------------------------------------------------ */
/* The definition table (icont's dlookup, as three functions) */

static unsigned hash(const char *name, size_t len) {
	unsigned t = 0;
	for (size_t i = 0; i < len; i++)
		t = 37 * t + (name[i] & 0xFF);
	return t % HTBINS;
}

/* dfind returns the definition of name (len bytes), or NULL. */
static cdefn *dfind(PP *pp, const char *name, size_t len) {
	if (len == 0)
		return NULL;
	for (cdefn *d = pp->cbin[hash(name, len)]; d; d = d->next)
		if (d->nlen == len && memcmp(name, d->s, len) == 0)
			return d;
	return NULL;
}

/* dremove deletes any definition of name. */
static void dremove(PP *pp, const char *name) {
	size_t len = strlen(name);
	if (len == 0)
		return;
	for (cdefn **p = &pp->cbin[hash(name, len)]; *p; p = &(*p)->next)
		if ((*p)->nlen == len && memcmp(name, (*p)->s, len) == 0) {
			*p = (*p)->next;
			return;
		}
}

/* dinstall defines name as val, reporting a different redefinition
 * (which keeps the old value). */
static void dinstall(PP *pp, const char *name, const char *val) {
	size_t nlen = strlen(name), vlen = strlen(val);
	if (nlen == 0)
		return;
	cdefn *d = dfind(pp, name, nlen);
	if (d) {
		if (strcmp(val, d->s + d->nlen) != 0)
			pfatal(pp, "value redefined", name);
		return;
	}
	unsigned h = hash(name, nlen);
	d = xcalloc(1, sizeof *d);
	d->nlen = nlen;
	d->vlen = vlen;
	d->s = xmalloc(nlen + vlen + 1);
	memcpy(d->s, name, nlen);
	memcpy(d->s + nlen, val, vlen + 1);
	d->next = pp->cbin[h];
	pp->cbin[h] = d;
}

/* ------------------------------------------------------------------ */
/* Files */

/*
 * rline() -- read the current file's next line into pp->line, ending
 *  it "\n\0" (a newline is supplied if the file's last line has none).
 *  Returns false at the end of the file.
 */
static bool rline(PP *pp) {
	infile *f = pp->curfile;
	if (f->at >= f->n)
		return false;
	const char *s = f->data + f->at;
	const char *nl = memchr(s, '\n', f->n - f->at);
	size_t len = nl ? (size_t)(nl - s) : f->n - f->at;
	f->at += len + (nl != NULL);
	pp->line.n = 0;
	bufadd(&pp->line, s, len);
	bufputc(&pp->line, '\n');
	return true;
}

/* readall reads a file whole; false if it cannot be opened. A read
 * error ends the file, as icont's getc. */
static bool readall(const char *name, Buf *b) {
	FILE *f = fopen(name, "r");
	if (!f)
		return false;
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(b, chunk, k);
	fclose(f);
	return true;
}

/*
 * ppopen(fname) -- open a new file for reading by the preprocessor.
 *
 *  Returns true if successful, false if open failed. Open calls may be
 *  nested. data, if not NULL, is the file's contents (n bytes).
 */
static bool ppopen(PP *pp, const char *fname, const char *data, size_t n) {
	for (infile *fs = pp->curfile; fs; fs = fs->prev)
		if (strcmp(fname, fs->fname) == 0) {
			pfatal(pp, "circular include", fname);
			return true; /* treat as success */
		}
	if (!data) {
		Buf b = {0};
		if (!readall(fname, &b))
			return false;
		data = b.b ? b.b : "";
		n = b.n;
	}
	infile *fs = xcalloc(1, sizeof *fs);
	fs->prev = pp->curfile;
	fs->fname = xstrndup(fname, strlen(fname));
	fs->data = data;
	fs->n = n;
	fs->ifdepth = pp->ifdepth;
	pp->curfile = fs;
	return true;
}

static bool canopen(const char *name) {
	FILE *f = fopen(name, "r");
	if (!f)
		return false;
	fclose(f);
	return true;
}

/*
 * pathfind(name) -- find an included file: name itself (in the
 *  current directory), then on the include path, as icont's pathfind
 *  (common/filepart.c): each directory, with a "/" added if it lacks
 *  one, and the last component of name. Returns NULL if not found.
 */
static char *pathfind(PP *pp, const char *name) {
	if (canopen(name))
		return xstrndup(name, strlen(name));
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	for (size_t i = 0; i < pp->opts->nincpath; i++) {
		const char *dir = pp->opts->incpath[i];
		size_t dn = strlen(dir);
		if (dn == 0)
			continue;
		Buf b = {0};
		bufputs(&b, dir);
		if (dir[dn - 1] != '/')
			bufputc(&b, '/');
		bufputs(&b, base);
		if (canopen(b.b))
			return b.b;
		free(b.b);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Directives */

static char *setline(PP *pp, char *s);

/*
 * define(s) -- handle $define directive.
 */
static const char *define(PP *pp, char *s) {
	char c, *name, *val;

	if (isalpha_(c = *s) || c == '_')
		s = getidt(name = s - 1, s); /* get name */
	else
		return "$define: missing name";
	if (*s == '(')
		return "$define: \"(\" after name requires preceding space";
	val = s = wskip(s);
	if (*s != '\0') {
		while ((c = *s) != '\0' && c != '#') { /* scan value */
			if (c == '"' || c == '\'') {
				s = matchqz(s);
				if (*s == '\0')
					return "$define: unterminated literal";
			}
			s++;
		}
		while (isspace_(s[-1])) /* trim trailing whitespace */
			s--;
	}
	*s = '\0';
	dinstall(pp, name, val); /* install in table */
	return NULL;
}

/*
 * undef(s) -- handle $undef directive.
 */
static const char *undef(PP *pp, char *s) {
	char c, *name;

	if (isalpha_(c = *s) || c == '_')
		s = getidt(name = s - 1, s); /* get name */
	else
		return "$undef: missing name";
	if (*wskip(s) != '\0')
		return "$undef: too many arguments";
	dremove(pp, name);
	return NULL;
}

/*
 * include(s) -- handle $include directive.
 */
static const char *include(PP *pp, char *s) {
	char *fname, *path;

	s = getfnm(fname = s - 1, s);
	if (*fname == '\0')
		return "$include: invalid file name";
	if (*wskip(s) != '\0')
		return "$include: too many arguments";
	if (!(path = pathfind(pp, fname)) || !ppopen(pp, path, NULL, 0))
		pfatal(pp, "cannot open", fname);
	return NULL;
}

/*
 * setline(s) -- handle $line (or #line) directive.
 */
static char *setline(PP *pp, char *s) {
	long n;
	char c;
	char *fname;

	if (!isdigit_(c = *s))
		return "$line: no line number";
	n = c - '0';

	while (isdigit_(c = *++s)) /* extract line number */
		n = 10 * n + c - '0';
	s = wskip(s); /* skip whitespace */

	if (isalpha_(c = *s) || c == '_' || c == '"') { /* if filename */
		s = getfnm(fname = s - 1, s);           /* extract it */
		if (*fname == '\0')
			return "$line: invalid file name";
	} else
		fname = NULL;

	if (*wskip(s) != '\0')
		return "$line: too many arguments";

	/* The directive line itself becomes line n (icont's "#line n"
	 * marker), so the next line read is n+1. */
	pp->curfile->lno = n;
	if (fname != NULL) /* also set filename if given */
		pp->curfile->fname = xstrndup(fname, strlen(fname));
	return NULL;
}

/* isdirective reports whether the line at buf (from its first
 * non-blank, p) is a $ directive: $ not followed by punctuation other
 * than a space. */
static bool isdirective(const char *p) { return *p == '$' && (!ispunct_((unsigned char)p[1]) || p[1] == ' '); }

/* ishashline reports whether the line is the #line form. */
static bool ishashline(const char *buf) { return strncmp(buf, "#line ", 6) == 0; }

/*
 * skipcode(doelse) -- skip code to $else (doelse=1) or $endif (=0).
 *
 *  Skipped lines are output empty. Nested $if sections are skipped
 *  whole (icont recurses; here the depth is counted), and $line is
 *  still obeyed.
 */
static void skipcode(PP *pp, bool doelse) {
	char c, *p, *cmd;
	int depth = 0; /* of nested $if sections being skipped */

	while (rline(pp)) {
		infile *f = pp->curfile;
		f->lno++; /* bump line number */
		emptyline(pp, f, f->lno);
		p = pp->line.b;

		/*
		 * Handle #line form encountered while skipping.
		 */
		if (ishashline(p)) {
			setline(pp, wskip(p + 5)); /* interpret #line, ignore errors */
			continue;
		}

		/*
		 * Check for any other kind of preprocessing directive.
		 */
		while (isspace_(c = *p))
			p++; /* find first nonwhite */
		if (!isdirective(p))
			continue; /* not a preprocessing directive */
		p = wskip(p + 1);          /* skip whitespace */
		p = getidt(cmd = p - 1, p); /* get command name */
		p = wskip(p);              /* skip whitespace */

		/*
		 * Check for a directive that needs special attention.
		 *  Deliberately accept any form of $if... as valid
		 *  in anticipation of possible future extensions;
		 *  this allows them to appear here if commented out.
		 */
		if (cmd[0] == 'i' && cmd[1] == 'f') {
			pp->ifdepth++;
			depth++; /* skip to its $endif */
		} else if (strcmp(cmd, "line") == 0)
			setline(pp, p); /* process $line, ignore errors */
		else if (strcmp(cmd, "endif") == 0 || (depth == 0 && doelse && strcmp(cmd, "else") == 0)) {
			/*
			 * Time to stop skipping (this section, or a nested one).
			 */
			if (*p != '\0')
				pfatal(pp, "extraneous arguments on $else/$endif", p);
			if (cmd[1] == 'n') /* if $endif */
				pp->ifdepth--;
			if (depth == 0)
				return;
			depth--;
		}
	}

	/*
	 *  At EOF, just return; main loop will report unterminated $if.
	 */
}

/*
 * ifxdef(s) -- handle $ifdef (if f is 1) or $ifndef (if f is 0).
 */
static const char *ifxdef(PP *pp, char *s, int f) {
	char c, *name;

	pp->ifdepth++;
	if (isalpha_(c = *s) || c == '_')
		s = getidt(name = s - 1, s); /* get name */
	else
		return "$ifdef/$ifndef: missing name";
	if (*wskip(s) != '\0')
		return "$ifdef/$ifndef: too many arguments";
	if ((dfind(pp, name, strlen(name)) != NULL) ^ f)
		skipcode(pp, true); /* skip to $else or $endif */
	return NULL;
}

/*
 * elsedir(s) -- handle $else by skipping to $endif.
 */
static const char *elsedir(PP *pp, char *s) {
	if (pp->ifdepth <= pp->curfile->ifdepth)
		return "unexpected $else";
	if (*s != '\0')
		pfatal(pp, "extraneous arguments on $else/$endif", s);
	skipcode(pp, false); /* skip the $else section */
	return NULL;
}

/*
 * endif(s) -- handle $endif.
 */
static const char *endif(PP *pp, char *s) {
	if (pp->ifdepth <= pp->curfile->ifdepth)
		return "unexpected $endif";
	if (*s != '\0')
		pfatal(pp, "extraneous arguments on $else/$endif", s);
	pp->ifdepth--;
	return NULL;
}

/*
 * ppdir(s, hashline) -- handle preprocessing directive.
 *
 *  s is the portion of the line following the $ (or the # of the #line
 *  form, whose errors are not reported).
 */
static void ppdir(PP *pp, char *s, bool hashline) {
	char *cmd;
	const char *errmsg;

	s = wskip(s);              /* skip whitespace */
	s = getidt(cmd = s - 1, s); /* get command name */
	s = wskip(s);              /* skip whitespace */

	if (strcmp(cmd, "define") == 0)
		errmsg = define(pp, s);
	else if (strcmp(cmd, "undef") == 0)
		errmsg = undef(pp, s);
	else if (strcmp(cmd, "ifdef") == 0)
		errmsg = ifxdef(pp, s, 1);
	else if (strcmp(cmd, "ifndef") == 0)
		errmsg = ifxdef(pp, s, 0);
	else if (strcmp(cmd, "else") == 0)
		errmsg = elsedir(pp, s);
	else if (strcmp(cmd, "endif") == 0)
		errmsg = endif(pp, s);
	else if (strcmp(cmd, "include") == 0)
		errmsg = include(pp, s);
	else if (strcmp(cmd, "line") == 0) {
		errmsg = setline(pp, s);
		if (hashline)
			errmsg = NULL; /* not reported from the #line form */
	} else if (strcmp(cmd, "error") == 0) {
		pfatal(pp, "explicit $error", s); /* issue msg with text */
		errmsg = NULL;
	} else {
		pfatal(pp, "invalid preprocessing directive", cmd);
		return;
	}
	if (errmsg != NULL)
		pfatal(pp, errmsg, NULL);
}

/* ------------------------------------------------------------------ */
/* Expansion */

/*
 * expand(s, n) -- write line s (n bytes) with definitions substituted,
 *  by ppch's loop: an identifier naming a definition not being expanded
 *  is replaced by its text, which is scanned in turn (a stack of
 *  pending buffers, not recursion); numbers, literals and comments are
 *  passed over.
 */
static void expand(PP *pp, const char *s, size_t n) {
	Buf *out = &pp->r->src;
	const char *bnxt = s, *bstop = s, *blim = s + n, *p;
	char c, f;
	cdefn *d;

	pp->bstack.n = 0;
	for (;;) {
		if (bnxt < bstop) { /* if characters ready to go */
			bufadd(out, bnxt, (size_t)(bstop - bnxt));
			bnxt = bstop;
			continue;
		}

		if (bnxt < blim) {
			/*
			 * There are characters in the buffer, but they haven't been
			 *  checked for substitutions yet.  Process either one id, if
			 *  that's what's next, or as much else as we can.
			 */
			f = *bnxt;
			if (isalpha_(f) || f == '_') {
				/*
				 * This is the first character of an identifier.  It could
				 *  be the name of a definition.  If so, the name will be
				 *  contiguous in this buffer.  Check it.
				 */
				p = bnxt + 1;
				while (p < blim && isidch(*p)) /* find end */
					p++;
				bstop = p; /* safe to consume through end */
				if ((d = dfind(pp, bnxt, (size_t)(p - bnxt))) == NULL || d->inuse)
					continue; /* not defined; just use it */
				/*
				 * We got a match.  Remove the token from the input stream and
				 *  push the replacement value.
				 */
				buffer b = {d, p, p, blim};
				VPUSH(pp->bstack, b);
				d->inuse = true;
				bnxt = bstop = d->s + d->nlen;
				blim = bnxt + d->vlen;
				continue; /* loop to preprocess */
			}
			/*
			 * Not an id.  Find the end of non-id stuff and mark it as
			 *  having been preprocessed.  This is where we skip over
			 *  string and cset literals to avoid processing them.
			 */
			p = bnxt;
			bstop = blim; /* unless an id is found */
			while (p < blim) {
				c = *p;
				if (isalpha_(c) || c == '_') { /* there's an id ahead */
					bstop = p;
					break;
				} else if (isdigit_(c)) /* numeric constant */
					p = nskip(p, blim);
				else if (c == '#') /* comment: skip to EOL */
					break;
				else if (c == '"' || c == '\'') { /* quoted literal */
					p = matchq(p, blim);   /* skip to end */
					if (p < blim)
						p++;
				} else
					p++; /* else advance one char */
			}
			continue;
		}

		/*
		 * The buffer is empty.  Revert to a previous buffer.
		 */
		if (pp->bstack.n == 0)
			return;
		buffer *b = &pp->bstack.v[--pp->bstack.n];
		b->defn->inuse = false;
		bnxt = b->ptr;
		bstop = b->stop;
		blim = b->lim;
	}
}

/* ------------------------------------------------------------------ */

PPResult *ipp_preprocess(const char *name, const char *src, size_t n, const PPOptions *opts) {
	PP pp = {.opts = opts};
	pp.r = xcalloc(1, sizeof *pp.r);
	bufputs(&pp.r->src, ""); /* never NULL */

	/*
	 * install predefined symbols
	 */
	for (size_t i = 0; i < opts->npredef; i++)
		dinstall(&pp, opts->prenames[i], opts->prevalues[i]);
	ppopen(&pp, name, src, n);

	while (pp.curfile) {
		if (!rline(&pp)) {
			/*
			 * The read hit EOF.
			 */
			infile *fs = pp.curfile;
			if (fs->ifdepth != pp.ifdepth) {
				pfatal(&pp, "unterminated $if", NULL);
				pp.ifdepth = fs->ifdepth;
			}
			pp.curfile = fs->prev; /* switch to previous file */
			continue;
		}
		infile *f = pp.curfile;
		char *buf = pp.line.b, *p = buf;
		long lno = ++f->lno; /* bump line number */
		while (isspace_(*p))
			p++; /* find first nonwhite */
		if (isdirective(p)) {
			emptyline(&pp, f, lno);
			ppdir(&pp, p + 1, false); /* handle preprocessor cmd */
		} else if (ishashline(buf)) {
			emptyline(&pp, f, lno);
			ppdir(&pp, buf + 1, true); /* handle #line form */
		} else {
			/*
			 * Not a preprocessor line; will need to scan for symbols.
			 */
			mark(&pp, f, lno);
			expand(&pp, buf, pp.line.n);
		}
	}
	free(pp.line.b);
	free(pp.bstack.v);
	return pp.r;
}
