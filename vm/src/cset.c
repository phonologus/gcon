/*
 * Csets, as Icon 9.5's: sets of the 256 byte values, in bits.
 */
#include "vm.h"

#include <string.h>

Cset *CsetAll, *CsetASCII, *CsetDigits, *CsetLcase, *CsetUcase, *CsetLetters;

Cset *newcset(void) { return gcnew(sizeof(Cset), T_CSET); }

int64_t cset_size(const Cset *c) {
	int64_t n = 0;
	for (int i = 0; i < 4; i++)
		n += __builtin_popcountll(c->bits[i]);
	return n;
}

bool cset_equal(const Cset *a, const Cset *b) { return memcmp(a->bits, b->bits, sizeof a->bits) == 0; }

Cset *csetop(int op, const Cset *a, const Cset *b) {
	Cset *r = newcset();
	for (int i = 0; i < 4; i++)
		r->bits[i] = op == '+' ? a->bits[i] | b->bits[i]
			   : op == '*' ? a->bits[i] & b->bits[i]
				       : a->bits[i] & ~b->bits[i];
	return r;
}

Cset *complement(const Cset *c) {
	Cset *r = newcset();
	for (int i = 0; i < 4; i++)
		r->bits[i] = ~c->bits[i];
	return r;
}

/* csetfrom returns the cset of the bytes of s. */
Cset *csetfrom(const char *s, int64_t n) {
	/* Four sets of bits, for bytes i, i+1, i+2 and i+3, merged at the
	 * end: one set would make each byte wait on the previous byte's
	 * store to the same word. */
	uint64_t b[4][4] = {{0}};
	const uint8_t *p = (const uint8_t *)s;
	int64_t i = 0;
	for (; i + 4 <= n; i += 4) {
		b[0][p[i] >> 6] |= (uint64_t)1 << (p[i] & 63);
		b[1][p[i + 1] >> 6] |= (uint64_t)1 << (p[i + 1] & 63);
		b[2][p[i + 2] >> 6] |= (uint64_t)1 << (p[i + 2] & 63);
		b[3][p[i + 3] >> 6] |= (uint64_t)1 << (p[i + 3] & 63);
	}
	for (; i < n; i++)
		b[0][p[i] >> 6] |= (uint64_t)1 << (p[i] & 63);
	Cset *c = newcset();
	for (int w = 0; w < 4; w++)
		c->bits[w] = b[0][w] | b[1][w] | b[2][w] | b[3][w];
	return c;
}

Cset *csetof(String s) { return csetfrom(s.s, s.n); }

/* cset_string returns c's members in order, made once (a cset never
 * changes). */
String cset_string(const Cset *c) {
	if (!c->str) {
		char buf[256];
		int k = 0;
		for (int x = 0; x < 256; x++)
			if (cset_has(c, x))
				buf[k++] = (char)x;
		String *s = gcnew(sizeof *s, T_STRHDR);
		*s = strcopy(buf, k);
		((Cset *)c)->str = s;
	}
	return *(String *)c->str;
}

static bool bitsare(const Cset *c, uint64_t b0, uint64_t b1, uint64_t b2, uint64_t b3) {
	return c->bits[0] == b0 && c->bits[1] == b1 && c->bits[2] == b2 && c->bits[3] == b3;
}

/* csetname_ returns the keyword whose cset c is, as iconx's csname. */
const char *csetname_(const Cset *c) {
	const uint64_t all = ~(uint64_t)0;
	const uint64_t digits = (uint64_t)0x3FF << 48;
	const uint64_t ucase = (uint64_t)0x3FFFFFF << 1, lcase = (uint64_t)0x3FFFFFF << 33;
	if (bitsare(c, 0, ucase | lcase, 0, 0)) return "&letters";
	if (bitsare(c, 0, lcase, 0, 0)) return "&lcase";
	if (bitsare(c, 0, ucase, 0, 0)) return "&ucase";
	if (bitsare(c, digits, 0, 0, 0)) return "&digits";
	if (bitsare(c, all, all, 0, 0)) return "&ascii";
	if (bitsare(c, all, all, all, all)) return "&cset";
	return NULL;
}

void cset_init(void) {
	CsetAll = newcset();
	CsetASCII = newcset();
	for (int i = 0; i < 4; i++)
		CsetAll->bits[i] = ~(uint64_t)0;
	CsetASCII->bits[0] = CsetASCII->bits[1] = ~(uint64_t)0;
	CsetDigits = csetfrom("0123456789", 10);
	CsetLcase = csetfrom("abcdefghijklmnopqrstuvwxyz", 26);
	CsetUcase = csetfrom("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26);
	CsetLetters = csetop('+', CsetLcase, CsetUcase);
	Cset **statics[] = {&CsetAll, &CsetASCII, &CsetDigits, &CsetLcase, &CsetUcase, &CsetLetters};
	for (size_t i = 0; i < sizeof statics / sizeof statics[0]; i++)
		gcroot((void **)statics[i]);
}
