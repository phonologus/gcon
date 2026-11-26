/*
 * Tokens (token.h): the tables, and lookups of reserved words,
 * operators and digraphs.
 */
#define TOKEN_TABLE
#include "token.h"

#include <stdio.h>
#include <string.h>

const char *tok_name(TokKind k) {
	static char buf[32];
	if ((int)k < T_NKINDS && tokinfo[k].name)
		return tokinfo[k].name;
	snprintf(buf, sizeof buf, "Kind(%d)", (int)k);
	return buf;
}

int tok_flags(TokKind k) { return (int)k < T_NKINDS ? tokinfo[k].flags : 0; }

static bool same(const char *a, const char *s, size_t n) {
	return strlen(a) == n && memcmp(a, s, n) == 0;
}

TokKind tok_lookup(const char *s, size_t n) {
	for (int k = T_keywordBeg + 1; k < T_keywordEnd; k++)
		if (same(tokinfo[k].name, s, n))
			return (TokKind)k;
	return T_IDENT;
}

/* Digraphs are alternate spellings the lexer accepts. */
static const struct { const char *s; TokKind k; } digraphs[] = {
	{"$(", T_LBRACE}, {"$)", T_RBRACE}, {"$<", T_LBRACK}, {"$>", T_RBRACK},
};

TokKind tok_operator_kind(const char *s, size_t n) {
	for (int k = T_operatorBeg + 1; k < T_operatorEnd; k++)
		if (same(tokinfo[k].name, s, n))
			return (TokKind)k;
	for (size_t i = 0; i < sizeof digraphs / sizeof digraphs[0]; i++)
		if (same(digraphs[i].s, s, n))
			return digraphs[i].k;
	return T_ILLEGAL;
}

bool tok_operator_prefix(const char *s, size_t n) {
	for (int k = T_operatorBeg + 1; k < T_operatorEnd; k++) {
		const char *o = tokinfo[k].name;
		if (strlen(o) >= n && memcmp(o, s, n) == 0)
			return true;
	}
	for (size_t i = 0; i < sizeof digraphs / sizeof digraphs[0]; i++)
		if (strlen(digraphs[i].s) >= n && memcmp(digraphs[i].s, s, n) == 0)
			return true;
	return false;
}
