/*
 * dump: the C front end's view of an Icon source file in the canonical
 * text form of the Go reference's tools/refdump, for the differential
 * tests (src/tests/diff.sh): the two must be identical.
 *
 *   dump lex FILE    tokens, trivia, decoded literals, lexical errors
 */
#include "../lexer.h"
#include "../lit.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Buf out;

/* esc: bytes outside printable ASCII, and the backslash, as \xHH */
static void esc(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c >= 0x7f || c == '\\')
			bufprintf(&out, "\\x%02x", c);
		else
			bufputc(&out, c);
	}
}

static void escz(const char *s) { esc(s, strlen(s)); }

static void lex(const char *src, size_t n) {
	Tokens ts = lex_tokenize(src, n);
	for (size_t k = 0; k < ts.n; k++) {
		Token *t = &ts.v[k];
		bufputs(&out, "T ");
		escz(tok_name(t->kind));
		bufprintf(&out, " %zu %zu", t->offset, t->len);
		if (t->implicit)
			bufputs(&out, " implicit");
		bufputc(&out, '\n');
		for (size_t i = 0; i < t->nlead; i++) {
			Trivia *tv = &t->lead[i];
			bufprintf(&out, "  t %s %zu ", trivia_name(tv->kind), tv->offset);
			esc(src + tv->offset, tv->len);
			bufputc(&out, '\n');
		}
		bufputs(&out, "  text ");
		esc(src + t->offset, t->len);
		bufputc(&out, '\n');
		const char *text = src + t->offset;
		if (t->kind == T_STRINGLIT || t->kind == T_CSETLIT) {
			char *s;
			LitError *err;
			size_t sn;
			LitUnit u;
			if ((err = lit_string(text, t->len, &s, &sn, &u))) {
				bufputs(&out, "  s err ");
				esc(err->msg, err->len);
			} else {
				bufprintf(&out, "  s %s ", lit_unit_name(u));
				esc(s, sn);
			}
			bufputc(&out, '\n');
			if (t->kind == T_CSETLIT) {
				LitCset c;
				if ((err = lit_cset(text, t->len, &c))) {
					bufputs(&out, "  c err ");
					esc(err->msg, err->len);
					bufputc(&out, '\n');
				} else {
					bufprintf(&out, "  c %s %zu\n", lit_unit_name(c.unit), c.nsegs);
					for (size_t i = 0; i < c.nsegs; i++) {
						bufputs(&out, "   seg ");
						esc(c.segs[i], c.seglens[i]);
						bufputc(&out, '\n');
					}
					for (size_t i = 0; i < c.nranges; i++)
						bufprintf(&out, "   range %d %d %s\n", (int)c.ranges[i].lo,
							(int)c.ranges[i].hi, c.ranges[i].lonebytes ? "true" : "false");
				}
			}
		} else if (t->kind == T_INTLIT || t->kind == T_REALLIT) {
			LitNumber num;
			LitError *err = lit_number(text, t->len, &num);
			if (err) {
				bufputs(&out, "  n err ");
				esc(err->msg, err->len);
				bufputc(&out, '\n');
			} else if (num.isreal) {
				uint64_t bits;
				memcpy(&bits, &num.real, 8);
				bufprintf(&out, "  n real %016" PRIx64 "\n", bits);
			} else if (num.isbig) {
				uint8_t *bytes;
				size_t nb = bignat_bytes(&num.big, &bytes);
				bufprintf(&out, "  n big %s ", bignat_decimal(&num.big));
				for (size_t i = 0; i < nb; i++)
					bufprintf(&out, "%02x", bytes[i]);
				bufputc(&out, '\n');
			} else
				bufprintf(&out, "  n int %" PRId64 "\n", num.i);
		}
	}
	for (size_t k = 0; k < ts.nerrs; k++) {
		bufprintf(&out, "E %zu ", ts.errs[k].offset);
		escz(ts.errs[k].msg);
		bufputc(&out, '\n');
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
}

int main(int argc, char **argv) {
	if (argc != 3 || strcmp(argv[1], "lex")) {
		fprintf(stderr, "usage: dump lex FILE\n");
		return 2;
	}
	FILE *f = fopen(argv[2], "rb");
	if (!f) {
		perror(argv[2]);
		return 1;
	}
	Buf src = {0};
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&src, chunk, k);
	fclose(f);
	lex(src.b ? src.b : "", src.n);
	return 0;
}
