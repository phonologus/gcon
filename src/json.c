/*
 * JSON (json.h). The reader keeps its own stack, as the rest of the
 * front end does: nesting has no ceiling but memory.
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct Reader {
	const char *s;
	size_t n, i;
	const char *err;
} Reader;

static void ws(Reader *r) {
	while (r->i < r->n && strchr(" \t\r\n", r->s[r->i]) && r->s[r->i])
		r->i++;
}

static int hexval(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hex4(Reader *r) {
	if (r->i + 4 > r->n)
		return -1;
	int v = 0;
	for (int k = 0; k < 4; k++) {
		int h = hexval((unsigned char)r->s[r->i + k]);
		if (h < 0)
			return -1;
		v = v * 16 + h;
	}
	r->i += 4;
	return v;
}

static void putrune(Buf *b, int32_t c) {
	uint8_t u[4];
	int w = utf8encode(u, c);
	bufadd(b, u, (size_t)w);
}

/* str reads a string, as Go's decoder: escapes decoded, invalid UTF-8
 * and lone surrogates replaced by U+FFFD. */
static bool str(Reader *r, char **out, size_t *outn) {
	r->i++; /* the quote */
	Buf b = {0};
	while (r->i < r->n) {
		unsigned char c = (unsigned char)r->s[r->i];
		if (c == '"') {
			r->i++;
			if (!b.b)
				bufputs(&b, "");
			*out = b.b, *outn = b.n;
			return true;
		}
		if (c < 0x20) {
			r->err = "invalid character in string literal";
			return false;
		}
		if (c == '\\') {
			if (r->i + 1 >= r->n)
				break;
			char e = r->s[r->i + 1];
			r->i += 2;
			switch (e) {
			case '"': case '\\': case '/':
				bufputc(&b, e);
				break;
			case 'b': bufputc(&b, '\b'); break;
			case 'f': bufputc(&b, '\f'); break;
			case 'n': bufputc(&b, '\n'); break;
			case 'r': bufputc(&b, '\r'); break;
			case 't': bufputc(&b, '\t'); break;
			case 'u': {
				int v = hex4(r);
				if (v < 0) {
					r->err = "invalid escape in string";
					return false;
				}
				if (v >= 0xD800 && v < 0xDC00) {
					/* a surrogate pair, or a lone surrogate */
					if (r->i + 6 <= r->n && r->s[r->i] == '\\' && r->s[r->i + 1] == 'u') {
						size_t save = r->i;
						r->i += 2;
						int w = hex4(r);
						if (w >= 0xDC00 && w < 0xE000) {
							putrune(&b, 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00));
							break;
						}
						r->i = save;
					}
					putrune(&b, RuneError);
				} else if (v >= 0xDC00 && v < 0xE000)
					putrune(&b, RuneError);
				else
					putrune(&b, v);
				break;
			}
			default:
				r->err = "invalid escape in string";
				return false;
			}
			continue;
		}
		if (c < RuneSelf) {
			bufputc(&b, c);
			r->i++;
			continue;
		}
		int w;
		int32_t rr = utf8decode((const uint8_t *)r->s + r->i, r->n - r->i, &w);
		if (rr == RuneError && w == 1)
			putrune(&b, RuneError);
		else
			bufadd(&b, r->s + r->i, (size_t)w);
		r->i += (size_t)w;
	}
	r->err = "unexpected end of JSON input";
	return false;
}

static JVal *newval(JKind k) {
	JVal *v = xcalloc(1, sizeof *v);
	v->kind = k;
	return v;
}

/* scalar reads a value that is not an array or object. */
static JVal *scalar(Reader *r) {
	const char *start = r->s + r->i;
	char c = r->s[r->i];
	JVal *v;
	if (c == '"') {
		v = newval(J_STR);
		if (!str(r, &v->s, &v->n))
			return NULL;
	} else if (c == 't' && r->n - r->i >= 4 && !memcmp(start, "true", 4)) {
		v = newval(J_BOOL);
		v->b = true;
		r->i += 4;
	} else if (c == 'f' && r->n - r->i >= 5 && !memcmp(start, "false", 5)) {
		v = newval(J_BOOL);
		r->i += 5;
	} else if (c == 'n' && r->n - r->i >= 4 && !memcmp(start, "null", 4)) {
		v = newval(J_NULL);
		r->i += 4;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		v = newval(J_NUM);
		size_t j = r->i;
		while (j < r->n && strchr("+-0123456789.eE", r->s[j]) && r->s[j])
			j++;
		char *t = xstrndup(start, j - r->i);
		char *end;
		v->num = strtod(t, &end);
		if (*end) {
			r->err = "invalid number";
			return NULL;
		}
		r->i = j;
	} else {
		r->err = "invalid character looking for beginning of value";
		return NULL;
	}
	v->raw = start;
	v->rawlen = (size_t)(r->s + r->i - start);
	return v;
}

JVal *json_parse(const char *text, size_t n, const char **err) {
	Reader r = {text, n, 0, NULL};
	VEC(JVal *) stk = {0}; /* open arrays and objects */
	char *key = NULL;      /* an object's member name, read and waiting for its value */
	JVal *root = NULL;
	ws(&r);
	for (;;) {
		JVal *top = stk.n ? stk.v[stk.n - 1] : NULL;
		ws(&r);
		if (r.i >= r.n) {
			r.err = "unexpected end of JSON input";
			break;
		}
		char c = r.s[r.i];
		/* closing, or separating, the current container */
		if (top && (c == ']' || c == '}')) {
			if ((c == ']') != (top->kind == J_ARR) || key) {
				r.err = "invalid character closing a value";
				break;
			}
			r.i++;
			top->rawlen = (size_t)(r.s + r.i - top->raw);
			stk.n--;
			goto valuedone;
		}
		if (top && top->kind == J_OBJ && !key) {
			if (top->keys.n) {
				if (c != ',') {
					r.err = "invalid character after object key:value pair";
					break;
				}
				r.i++;
				ws(&r);
				if (r.i >= r.n)
					continue;
				c = r.s[r.i];
			}
			if (c != '"') {
				r.err = "invalid character looking for beginning of object key string";
				break;
			}
			size_t kn;
			if (!str(&r, &key, &kn))
				break;
			ws(&r);
			if (r.i >= r.n || r.s[r.i] != ':') {
				r.err = "invalid character after object key";
				break;
			}
			r.i++;
			continue;
		}
		if (top && top->kind == J_ARR && top->items.n) {
			if (c != ',') {
				r.err = "invalid character after array element";
				break;
			}
			r.i++;
			ws(&r);
			if (r.i >= r.n)
				continue;
			c = r.s[r.i];
		}
		{
			/* a value */
			JVal *v;
			if (c == '[' || c == '{') {
				v = newval(c == '[' ? J_ARR : J_OBJ);
				v->raw = r.s + r.i;
				r.i++;
			} else if (!(v = scalar(&r)))
				break;
			if (top) {
				VPUSH(top->items, v);
				if (top->kind == J_OBJ) {
					VPUSH(top->keys, key);
					key = NULL;
				}
			} else
				root = v;
			if (v->kind == J_ARR || v->kind == J_OBJ) {
				VPUSH(stk, v);
				continue;
			}
		}
	valuedone:
		if (!stk.n) {
			ws(&r);
			if (r.i != r.n) {
				r.err = "invalid character after top-level value";
				break;
			}
			free(stk.v);
			return root;
		}
	}
	free(stk.v);
	*err = r.err ? r.err : "invalid JSON";
	return NULL;
}

JVal *json_get(JVal *v, const char *path) {
	while (v && *path) {
		const char *dot = strchr(path, '.');
		size_t n = dot ? (size_t)(dot - path) : strlen(path);
		if (v->kind != J_OBJ)
			return NULL;
		JVal *found = NULL;
		/* the last of duplicate keys wins, as in Go */
		for (size_t i = 0; i < v->keys.n; i++)
			if (strlen(v->keys.v[i]) == n && !memcmp(v->keys.v[i], path, n))
				found = v->items.v[i];
		v = found;
		path = dot ? dot + 1 : path + n;
	}
	return v;
}

const char *json_str(JVal *v, const char *path) {
	v = json_get(v, path);
	return v && v->kind == J_STR ? v->s : "";
}

long json_int(JVal *v, const char *path) {
	v = json_get(v, path);
	return v && v->kind == J_NUM ? (long)v->num : 0;
}

bool json_bool(JVal *v, const char *path) {
	v = json_get(v, path);
	return v && v->kind == J_BOOL && v->b;
}

/* ------------------------------------------------------------------ */
/* Writing */

static const char hexdig[] = "0123456789abcdef";

void json_string(Buf *b, const char *s, size_t n) {
	bufputc(b, '"');
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c < RuneSelf) {
			switch (c) {
			case '\\': case '"':
				bufputc(b, '\\');
				bufputc(b, c);
				break;
			case '\b': bufputs(b, "\\b"); break;
			case '\f': bufputs(b, "\\f"); break;
			case '\n': bufputs(b, "\\n"); break;
			case '\r': bufputs(b, "\\r"); break;
			case '\t': bufputs(b, "\\t"); break;
			default:
				if (c < 0x20 || c == '<' || c == '>' || c == '&') {
					bufputs(b, "\\u00");
					bufputc(b, hexdig[c >> 4]);
					bufputc(b, hexdig[c & 0xF]);
				} else
					bufputc(b, c);
			}
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode((const uint8_t *)s + i, n - i, &w);
		if (r == RuneError && w == 1)
			bufputs(b, "\\ufffd");
		else if (r == 0x2028 || r == 0x2029) {
			bufputs(b, "\\u202");
			bufputc(b, hexdig[r & 0xF]);
		} else
			bufadd(b, s + i, (size_t)w);
		i += (size_t)w;
	}
	bufputc(b, '"');
}

void json_stringz(Buf *b, const char *s) { json_string(b, s, strlen(s)); }

/* json_compact: the input text without insignificant space, strings
 * kept as written but with <, >, &, U+2028 and U+2029 escaped (as Go's
 * compact with HTML escaping, which it applies to a RawMessage). */
void json_compact(Buf *b, JVal *v) {
	const char *s = v->raw;
	size_t n = v->rawlen;
	bool instr = false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (instr) {
			if (c == '\\' && i + 1 < n) {
				bufputc(b, c);
				bufputc(b, s[++i]);
				continue;
			}
			if (c == '"')
				instr = false;
			if (c == '<' || c == '>' || c == '&') {
				bufputs(b, "\\u00");
				bufputc(b, hexdig[c >> 4]);
				bufputc(b, hexdig[c & 0xF]);
				continue;
			}
			if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
			    ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
				bufputs(b, "\\u202");
				bufputc(b, hexdig[(unsigned char)s[i + 2] & 0xF]);
				i += 2;
				continue;
			}
			bufputc(b, c);
			continue;
		}
		if (c == '"')
			instr = true;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		bufputc(b, c);
	}
}
