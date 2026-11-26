/*
 * JSON for the language server: a reader into a tree, and a writer
 * that writes as Go's encoding/json does (compact; strings escaped as
 * Go escapes them, HTML characters included; invalid UTF-8 replaced
 * by U+FFFD), as iconls, which it was ported from, writes it.
 */
#ifndef GCON_JSON_H
#define GCON_JSON_H

#include "util.h"

typedef enum JKind { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal JVal;
struct JVal {
	JKind kind;
	bool b;
	double num;      /* J_NUM */
	const char *raw; /* J_NUM: the number's text; any value: its text in the input */
	size_t rawlen;
	char *s;         /* J_STR: decoded (UTF-8) */
	size_t n;        /* J_STR: its length */
	VEC(JVal *) items; /* J_ARR, or J_OBJ's values */
	VEC(char *) keys;  /* J_OBJ, in input order */
};

/* json_parse parses text (n bytes); NULL, with an error message in
 * *err, if it is not JSON. */
JVal *json_parse(const char *text, size_t n, const char **err);

/* Members and fields: NULL (or zero, false, "") if absent or of another
 * kind. Paths are of keys separated by "." ("textDocument.uri"). */
JVal *json_get(JVal *v, const char *path);
const char *json_str(JVal *v, const char *path); /* "" if absent */
long json_int(JVal *v, const char *path);
bool json_bool(JVal *v, const char *path);

/* json_compact writes v's input text compactly (as Go re-encodes a
 * RawMessage). */
void json_compact(Buf *b, JVal *v);

/* Writing: strings as Go's encoding/json writes them. */
void json_string(Buf *b, const char *s, size_t n);
void json_stringz(Buf *b, const char *s);

#endif
