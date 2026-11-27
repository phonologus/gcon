/*
 * Literal tokens decoded (the Go reference's internal/lit): strings and
 * csets, with icont's escapes (getstring in src/common/yylex.h, the
 * table in src/h/esctab.h) and gcon's \u and \U (a code point's UTF-8
 * bytes); numbers. A string or cset is its bytes, as in Icon 9.5.
 */
#ifndef GCON_LIT_H
#define GCON_LIT_H

#include "util.h"

/* An error decoding a literal: its message may quote the token, NUL
 * bytes and all, so it has a length. Functions return NULL for none. */
typedef struct LitError {
	char *msg;
	size_t len;
} LitError;

/* lit_string decodes a string or cset literal token (quotes included)
 * into *out (malloc'd, n bytes in *outn). */
LitError *lit_string(const char *tok, size_t n, char **out, size_t *outn);

/* A range in a cset literal: bytes. */
typedef struct LitRange {
	int32_t lo, hi;
} LitRange;

/* lit_cset decodes a cset literal token: the text between its ranges,
 * as segments, and the ranges. */
typedef struct LitCset {
	char **segs;
	size_t *seglens;
	size_t nsegs;
	LitRange *ranges;
	size_t nranges;
} LitCset;

LitError *lit_cset(const char *tok, size_t n, LitCset *out);

/* A uset literal's members: the ASCII characters and the bytes from
 * 0x80 in bits, and code-point ranges (each from 0x80, unsorted, as
 * written: the compiler normalizes them). */
typedef struct LitUrange {
	int32_t lo, hi;
} LitUrange;
typedef struct LitUset {
	uint64_t bits[4];
	LitUrange *ranges;
	size_t nranges;
} LitUset;

/* lit_uset decodes a uset literal token, u'...' (the u included): a
 * UTF-8 character or a \u or \U escape is a code point, any other
 * byte from 0x80 a byte; a range's ends are both code points or both
 * bytes (ASCII is either). */
LitError *lit_uset(const char *tok, size_t n, LitUset *out);

/* A numeric literal: an integer (in i, or big if beyond int64) or a
 * real. */
typedef struct LitNumber {
	int64_t i;
	BigNat big;
	bool isbig;
	double real;
	bool isreal;
} LitNumber;

LitError *lit_number(const char *tok, size_t n, LitNumber *out);

#endif
