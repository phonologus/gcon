/*
 * The lexer (lexer.h), transcribed from the Go reference's
 * internal/lexer, whose behaviour was checked against icont's lexer over
 * the reference corpus.
 */
#include "lexer.h"

#include <string.h>

const char *trivia_name(TriviaKind k) {
	switch (k) {
	case TV_SPACE: return "Space";
	case TV_NEWLINE: return "Newline";
	case TV_COMMENT: return "Comment";
	case TV_DIRECTIVE: return "Directive";
	}
	return "TriviaKind(?)";
}

typedef struct Lexer {
	const char *src;
	size_t n, pos;
	Tokens out;
	bool atLineStart; /* only whitespace seen since the last newline */
	bool sawNewline;  /* a newline occurred since the last significant token */
	bool lastEnder;   /* the last significant token was an Ender */
} Lexer;

static void errorf(Lexer *l, size_t off, const char *msg) {
	Tokens *t = &l->out;
	if (t->nerrs == t->caperrs) {
		t->caperrs = t->caperrs ? 2 * t->caperrs : 8;
		t->errs = xrealloc(t->errs, t->caperrs * sizeof *t->errs);
	}
	t->errs[t->nerrs++] = (LexError){off, msg};
}

static void push(Lexer *l, Token tok) {
	Tokens *t = &l->out;
	if (t->n == t->cap) {
		t->cap = t->cap ? 2 * t->cap : 64;
		t->v = xrealloc(t->v, t->cap * sizeof *t->v);
	}
	t->v[t->n++] = tok;
}

static unsigned char peek(Lexer *l, size_t i) {
	return l->pos + i < l->n ? (unsigned char)l->src[l->pos + i] : 0;
}

static bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }
static bool isDigit(unsigned char c) { return '0' <= c && c <= '9'; }
static bool isLetter(unsigned char c) { return 'a' <= (c | 0x20) && (c | 0x20) <= 'z'; }
static bool isAlnum(unsigned char c) { return isLetter(c) || isDigit(c); }

static int digitVal(unsigned char c) {
	if (isDigit(c))
		return c - '0';
	return (int)((c | 0x20) - 'a') + 10;
}

/* isDirectiveStart: c, after a '$' that begins a line, makes the line a
 * preprocessor directive (ipp.c: !ispunct(c) || c == ' '). */
static bool isDirectiveStart(unsigned char c) {
	bool isPunct = c > ' ' && c < 0x7f && !isAlnum(c);
	return !isPunct;
}

static void toEOL(Lexer *l) {
	const char *nl = memchr(l->src + l->pos, '\n', l->n - l->pos);
	l->pos = nl ? (size_t)(nl - l->src) : l->n;
}

/* trivia consumes whitespace, comments and directive lines. */
static Trivia *trivia(Lexer *l, size_t *count) {
	VEC(Trivia) out = {0};
	while (l->pos < l->n) {
		size_t start = l->pos;
		TriviaKind kind;
		unsigned char c = (unsigned char)l->src[l->pos];
		if (c == '\n') {
			l->pos++;
			kind = TV_NEWLINE;
			l->sawNewline = true;
			l->atLineStart = true;
		} else if (isSpace(c)) {
			while (l->pos < l->n && isSpace((unsigned char)l->src[l->pos]))
				l->pos++;
			kind = TV_SPACE;
		} else if (c == '#') {
			toEOL(l);
			kind = TV_COMMENT;
		} else if (c == '$' && l->atLineStart && isDirectiveStart(peek(l, 1))) {
			toEOL(l);
			kind = TV_DIRECTIVE;
		} else
			break;
		VPUSH(out, ((Trivia){kind, start, l->pos - start}));
	}
	*count = out.n;
	return out.v;
}

/* number follows getnum in icont's src/common/yylex.h, including its
 * treatment of a leading '.' (a real literal if a digit follows,
 * otherwise the DOT operator). */
static TokKind number(Lexer *l) {
	size_t start = l->pos;
	unsigned char c = (unsigned char)l->src[l->pos];
	l->pos++;
	int state = 0, radix = 0;
	bool isReal = false;
	if (c == '.')
		state = 7, isReal = true;
	else
		radix = c - '0';
	for (;;) {
		c = l->pos < l->n ? (unsigned char)l->src[l->pos] : 0;
		switch (state) {
		case 0: /* integer part */
			if (isDigit(c)) {
				if (radix < 1000)
					radix = radix * 10 + (c - '0');
				l->pos++;
				continue;
			}
			if (c == '.') {
				state = 1, isReal = true;
				l->pos++;
				continue;
			}
			if (c == 'e' || c == 'E') {
				state = 2, isReal = true;
				l->pos++;
				continue;
			}
			if (c == 'r' || c == 'R') {
				if (radix < 2 || radix > 36)
					errorf(l, start, "invalid radix for integer literal");
				state = 5;
				l->pos++;
				continue;
			}
			break;
		case 1: /* fractional part */
			if (isDigit(c)) {
				l->pos++;
				continue;
			}
			if (c == 'e' || c == 'E') {
				state = 2;
				l->pos++;
				continue;
			}
			break;
		case 2:
		case 3: /* optional exponent sign, then first exponent digit */
			if (state == 2 && (c == '+' || c == '-')) {
				state = 3;
				l->pos++;
				continue;
			}
			if (isDigit(c)) {
				state = 4;
				l->pos++;
				continue;
			}
			errorf(l, start, "invalid real literal");
			break;
		case 4: /* remaining exponent digits */
			if (isDigit(c)) {
				l->pos++;
				continue;
			}
			break;
		case 5: /* first digit after r */
			if (isAlnum(c) && digitVal(c) < radix) {
				state = 6;
				l->pos++;
				continue;
			}
			errorf(l, start, "invalid integer literal");
			break;
		case 6: /* remaining digits after r */
			if (isAlnum(c)) {
				if (digitVal(c) >= radix) {
					errorf(l, start, "invalid digit in integer literal");
					radix = 36; /* as icont: report once */
				}
				l->pos++;
				continue;
			}
			break;
		case 7: /* began with '.' */
			if (isDigit(c)) {
				state = 1;
				l->pos++;
				continue;
			}
			return T_DOT;
		}
		break;
	}
	return isReal ? T_REALLIT : T_INTLIT;
}

/* quoted follows getstring in icont's yylex.h: escapes, and the rule
 * that '_' as the last non-blank on a line continues the literal at the
 * first non-blank of the next line. An unescaped newline otherwise ends
 * an (unclosed) literal. */
static TokKind quoted(Lexer *l) {
	size_t start = l->pos;
	char q = l->src[l->pos];
	TokKind kind = q == '\'' ? T_CSETLIT : T_STRINGLIT;
	l->pos++;
	bool cont = false; /* '_' seen with only blanks after it */
	while (l->pos < l->n) {
		unsigned char c = (unsigned char)l->src[l->pos];
		if (c == (unsigned char)q) {
			l->pos++;
			return kind;
		}
		if (c == '\n')
			break;
		if (c == '_')
			cont = true;
		else if (!isSpace(c))
			cont = false;
		l->pos++;
		if (c == '\\' && l->pos < l->n) {
			/* The escaped character is taken literally, even a quote
			 * or newline; "\^c" takes one more. */
			unsigned char e = (unsigned char)l->src[l->pos];
			l->pos++;
			if (e == '^' && l->pos < l->n)
				l->pos++;
		}
		if (l->pos < l->n && l->src[l->pos] == '\n' && cont) {
			while (l->pos < l->n && (isSpace((unsigned char)l->src[l->pos]) || l->src[l->pos] == '\n'))
				l->pos++;
		}
	}
	errorf(l, start, "unclosed quote");
	return kind;
}

/* operator takes the longest run of characters that is a prefix of some
 * operator, as icont's getopr does (without backtracking). */
static TokKind operator(Lexer *l) {
	size_t start = l->pos, end = start;
	while (end < l->n && tok_operator_prefix(l->src + start, end + 1 - start))
		end++;
	if (end == start) {
		int w;
		utf8decode((const uint8_t *)l->src + start, l->n - start, &w);
		l->pos += (size_t)w;
		errorf(l, start, "invalid character");
		return T_ILLEGAL;
	}
	l->pos = end;
	TokKind k = tok_operator_kind(l->src + start, end - start);
	if (k != T_ILLEGAL)
		return k;
	errorf(l, start, "invalid character");
	return T_ILLEGAL;
}

static TokKind scan(Lexer *l) {
	unsigned char c = (unsigned char)l->src[l->pos];
	if (isLetter(c) || c == '_') {
		size_t start = l->pos;
		while (l->pos < l->n && (isAlnum((unsigned char)l->src[l->pos]) || l->src[l->pos] == '_'))
			l->pos++;
		/* gcon's uset literal: u directly followed by a cset literal (in
		 * 9.5 an identifier followed by a literal is a syntax error) */
		if (l->pos - start == 1 && c == 'u' && l->pos < l->n && l->src[l->pos] == '\'') {
			quoted(l);
			return T_USETLIT;
		}
		return tok_lookup(l->src + start, l->pos - start);
	}
	if (isDigit(c) || c == '.')
		return number(l);
	if (c == '"' || c == '\'')
		return quoted(l);
	return operator(l);
}

static size_t triviaLen(const Trivia *ts, size_t n) {
	size_t k = 0;
	for (size_t i = 0; i < n; i++)
		k += ts[i].len;
	return k;
}

Tokens lex_tokenize(const char *src, size_t n) {
	Lexer l = {.src = src, .n = n, .atLineStart = true};
	for (;;) {
		size_t nlead;
		Trivia *lead = trivia(&l, &nlead);
		size_t start = l.pos;
		if (start >= l.n) {
			push(&l, (Token){T_EOF, start, 0, lead, nlead, false});
			return l.out;
		}
		TokKind kind = scan(&l);
		Token tok = {kind, start, l.pos - start, lead, nlead, false};
		if (kind == T_ILLEGAL) {
			/* icont skips bad characters without disturbing the
			 * semicolon-insertion state */
			push(&l, tok);
			continue;
		}
		if (l.sawNewline && l.lastEnder && tok_beginner(kind)) {
			size_t at = start - triviaLen(lead, nlead);
			push(&l, (Token){T_SEMICOL, at, 0, NULL, 0, true});
		}
		push(&l, tok);
		l.sawNewline = false;
		l.lastEnder = tok_ender(kind);
		l.atLineStart = false;
	}
}

/* ---- Lines ---- */

Lines lines_new(const char *src, size_t n) {
	VEC(size_t) s = {0};
	VPUSH(s, (size_t)0);
	for (size_t i = 0; i < n; i++)
		if (src[i] == '\n')
			VPUSH(s, i + 1);
	return (Lines){s.v, s.n};
}

void lines_position(const Lines *ls, size_t off, int *line, int *col) {
	/* the last line start <= off (sort.Search for the first > off) */
	size_t lo = 0, hi = ls->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (ls->starts[mid] > off)
			hi = mid;
		else
			lo = mid + 1;
	}
	size_t i = lo - 1;
	*line = (int)i + 1;
	*col = (int)(off - ls->starts[i]);
}
