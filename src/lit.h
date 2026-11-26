/*
 * Literal tokens decoded (the Go reference's internal/lit): strings and
 * csets, with icont's escapes (getstring in src/common/yylex.h, the
 * table in src/h/esctab.h) and gcon's \u and \U; numbers. The unit of a
 * string or cset follows gcon's docs/design.md ("Strings and Unicode").
 */
#ifndef GCON_LIT_H
#define GCON_LIT_H

#include "util.h"

typedef enum LitUnit { U_ASCII, U_TEXT, U_BYTES } LitUnit;

const char *lit_unit_name(LitUnit u); /* "ascii", "text", "bytes" */

/* An error decoding a literal: its message may quote the token, NUL
 * bytes and all, so it has a length. Functions return NULL for none. */
typedef struct LitError {
	char *msg;
	size_t len;
} LitError;

/* lit_string decodes a string or cset literal token (quotes included)
 * into *out (malloc'd, n bytes in *outn) and its unit. */
LitError *lit_string(const char *tok, size_t n, char **out, size_t *outn, LitUnit *unit);

/* A range in a cset literal: code points, or bytes (lonebytes). */
typedef struct LitRange {
	int32_t lo, hi;
	bool lonebytes;
} LitRange;

/* lit_cset decodes a cset literal token: the text between its ranges,
 * as segments (read separately, so that bytes cannot fuse across a
 * range), the ranges, and the unit. */
typedef struct LitCset {
	char **segs;
	size_t *seglens;
	size_t nsegs;
	LitRange *ranges;
	size_t nranges;
	LitUnit unit;
} LitCset;

LitError *lit_cset(const char *tok, size_t n, LitCset *out);

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
