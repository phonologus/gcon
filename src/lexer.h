/*
 * The lexer (the Go reference's internal/lexer): Icon source to a
 * lossless token stream. Unlike icont's lexer (src/common/yylex.h), it
 * keeps comments, whitespace and preprocessor directive lines as trivia
 * attached to the following token, records byte offsets, and marks the
 * semicolons it inserts (an Ender at the end of a line followed by a
 * Beginner on the next) as implicit. Concatenating every token's
 * leading trivia and text reproduces the input exactly. Directives are
 * not expanded.
 */
#ifndef GCON_LEXER_H
#define GCON_LEXER_H

#include "token.h"
#include "util.h"

typedef enum TriviaKind { TV_SPACE, TV_NEWLINE, TV_COMMENT, TV_DIRECTIVE } TriviaKind;

const char *trivia_name(TriviaKind k);

typedef struct Trivia {
	TriviaKind kind;
	size_t offset, len; /* the text is src[offset, offset+len) */
} Trivia;

/* A token and the trivia before it. Implicit semicolons have kind
 * T_SEMICOL, implicit set, no text and no trivia; their offset is the
 * end of the token before. The last token is T_EOF, with any trailing
 * trivia. */
typedef struct Token {
	TokKind kind;
	size_t offset, len;
	Trivia *lead;
	size_t nlead;
	bool implicit;
} Token;

/* A lexical error; the messages are icont's where icont has one. */
typedef struct LexError {
	size_t offset;
	const char *msg;
} LexError;

typedef struct Tokens {
	Token *v;
	size_t n, cap;
	LexError *errs;
	size_t nerrs, caperrs;
} Tokens;

/* lex_tokenize splits src (n bytes) into tokens. It never fails:
 * malformed input gives T_ILLEGAL tokens or best-effort literals, with
 * the problems in errs. */
Tokens lex_tokenize(const char *src, size_t n);

/* Line positions: 1-based line, 0-based byte column of an offset. */
typedef struct Lines {
	size_t *starts;
	size_t n;
} Lines;

Lines lines_new(const char *src, size_t n);
void lines_position(const Lines *ls, size_t off, int *line, int *col);

#endif
