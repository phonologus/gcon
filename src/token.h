/*
 * The lexical tokens of Icon 9.5 and the flags that drive semicolon
 * insertion (the Go reference's internal/token). The tables are icont's
 * src/common/lextab.h; several binary-only operators (||, **, ==, ~=,
 * ...) are Beginners there, because the grammar accepts "||x" as
 * "|(|x)": they are not to be "corrected".
 */
#ifndef GCON_TOKEN_H
#define GCON_TOKEN_H

#include <stdbool.h>
#include <stddef.h>

enum { TF_BEGINNER = 1, TF_ENDER = 2 };

struct TokInfo {
	const char *name; /* spelling (keywords, operators) or description */
	int flags;
};

#include "tokens.inc"

typedef enum TokKind TokKind;

const char *tok_name(TokKind k); /* as the Go Kind.String */
int tok_flags(TokKind k);
static inline bool tok_beginner(TokKind k) { return tok_flags(k) & TF_BEGINNER; }
static inline bool tok_ender(TokKind k) { return tok_flags(k) & TF_ENDER; }
static inline bool tok_keyword(TokKind k) { return T_keywordBeg < k && k < T_keywordEnd; }
static inline bool tok_operator(TokKind k) { return T_operatorBeg < k && k < T_operatorEnd; }

TokKind tok_lookup(const char *s, size_t n); /* a reserved word, or T_IDENT */
TokKind tok_operator_kind(const char *s, size_t n); /* an operator or digraph, or T_ILLEGAL */
bool tok_operator_prefix(const char *s, size_t n); /* a prefix of an operator or digraph */

#endif
