/*
 * The catalogue of built-ins (builtin.h).
 */
#include "builtin.h"

#include <string.h>

#include "builtin.inc"

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* cmp compares name (n bytes) with the NUL-terminated s, as strcmp. */
static int cmp(const char *name, size_t n, const char *s) {
	size_t m = strlen(s);
	int c = memcmp(name, s, n < m ? n : m);
	return c ? c : (n > m) - (n < m);
}

#define BSEARCH(tab)                                                   \
	size_t lo = 0, hi = LEN(tab);                                  \
	while (lo < hi) {                                              \
		size_t mid = (lo + hi) / 2;                            \
		int c = cmp(name, n, tab[mid].name);                   \
		if (c == 0)                                            \
			return &tab[mid];                              \
		if (c < 0)                                             \
			hi = mid;                                      \
		else                                                   \
			lo = mid + 1;                                  \
	}                                                              \
	return NULL;

const BuiltinFunc *builtin_function(const char *name, size_t n) { BSEARCH(functions) }
const Keyword *builtin_keyword(const char *name, size_t n) { BSEARCH(keywords) }

const Predefined *builtin_predefined(const char *name, size_t n) {
	for (size_t i = 0; i < LEN(predefined); i++)
		if (cmp(name, n, predefined[i].name) == 0)
			return &predefined[i];
	return NULL;
}

const BuiltinFunc *builtin_functions(size_t *n) {
	*n = LEN(functions);
	return functions;
}

const Keyword *builtin_keywords(size_t *n) {
	*n = LEN(keywords);
	return keywords;
}
