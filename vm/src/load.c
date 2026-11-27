/*
 * The loader: decodes a built program (gcon's internal/vm/encode.go;
 * gcon's design/design.md, "The built program's format") and checks it as
 * gcon's vm.Decode and vm.Validate do, with the same messages, so that
 * a damaged file, or one built by another version of gcon, is reported
 * rather than run.
 *
 * As in gcon, a decoder records the first error and then reads
 * nothing more: every read after a failure returns zero, so decoding
 * code reads straight through and checks once at the end.
 */
#include "gcon-vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char Magic[8] = {'\x89', 'G', 'C', 'N', '\r', '\n', '\x1a', '\n'};

/* The marker ends a built program's shell header (gcon's
 * internal/run/build.go); the encoded program follows it at once. */
static const char marker[] = "[gcon program follows]\n";

/* encoded returns the encoded program within data, and its length: in a
 * built program, a file starting "#!" in which a line holding the
 * marker is followed by Magic; or the whole of data if it starts with
 * Magic (bytecode handed over by gcon run). NULL if neither. */
const uint8_t *encoded(const uint8_t *data, size_t n, size_t *len) {
	if (n >= sizeof Magic && memcmp(data, Magic, sizeof Magic) == 0) {
		*len = n;
		return data;
	}
	if (n < 2 || data[0] != '#' || data[1] != '!')
		return NULL;
	size_t m = strlen(marker);
	for (size_t i = 0; i + 1 + m + sizeof Magic <= n; i++) {
		if (data[i] == '\n' && memcmp(data + i + 1, marker, m) == 0 &&
		    memcmp(data + i + 1 + m, Magic, sizeof Magic) == 0) {
			*len = n - (i + 1 + m);
			return data + i + 1 + m;
		}
	}
	return NULL;
}

typedef struct Decoder {
	const uint8_t *b;
	size_t n, pos;
	Buf *err;
	Program *p;
} Decoder;

/* Messages are buffers, not C strings: they can quote names from the
 * file, which may hold any bytes, NUL included, and are written as
 * gcon writes them. */

static Buf *vmsg(const char *fmt, va_list ap) {
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	char *s = xmalloc((size_t)n + 1);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	Buf *b = xcalloc(1, sizeof *b);
	bufadd(b, s, (size_t)n);
	free(s);
	return b;
}

static Buf *msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static Buf *msg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	Buf *b = vmsg(fmt, ap);
	va_end(ap);
	return b;
}

static void freemsg(Buf *b) {
	if (b) {
		free(b->b);
		free(b);
	}
}

/* wrap appends inner to outer and returns outer. */
static Buf *wrap(Buf *outer, Buf *inner) {
	bufadd(outer, inner->b, inner->n);
	freemsg(inner);
	return outer;
}

static void fail(Decoder *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Decoder *d, const char *fmt, ...) {
	if (d->err)
		return;
	va_list ap;
	va_start(ap, fmt);
	d->err = vmsg(fmt, ap);
	va_end(ap);
}

static void short_(Decoder *d) { fail(d, "unexpected end of data"); }

/* uvarint reads an unsigned varint as Go's binary.Uvarint: at most ten
 * bytes, the tenth at most 1; otherwise, or if the data ends, an error. */
static uint64_t uvarint(Decoder *d) {
	if (d->err)
		return 0;
	uint64_t x = 0;
	unsigned s = 0;
	for (size_t i = 0; d->pos + i < d->n; i++) {
		uint8_t c = d->b[d->pos + i];
		if (i == 10)
			break;
		if (c < 0x80) {
			if (i == 9 && c > 1)
				break;
			d->pos += i + 1;
			return x | (uint64_t)c << s;
		}
		x |= (uint64_t)(c & 0x7F) << s;
		s += 7;
	}
	short_(d);
	return 0;
}

static int64_t varint(Decoder *d) {
	uint64_t u = uvarint(d);
	int64_t x = (int64_t)(u >> 1);
	return u & 1 ? ~x : x;
}

/* small reads an unsigned integer that must fit in an int32. */
static int32_t small(Decoder *d) {
	uint64_t x = uvarint(d);
	if (x > INT32_MAX) {
		fail(d, "integer %llu out of range", (unsigned long long)x);
		return 0;
	}
	return (int32_t)x;
}

/* count reads a number of elements, each at least one byte long, so no
 * more than there are bytes left. */
static int32_t count(Decoder *d) {
	int32_t n = small(d);
	if ((size_t)n > d->n - d->pos) {
		fail(d, "count %d exceeds the data", n);
		return 0;
	}
	return n;
}

static uint8_t byte(Decoder *d) {
	if (d->err)
		return 0;
	if (d->pos >= d->n) {
		short_(d);
		return 0;
	}
	return d->b[d->pos++];
}

/* take returns the next n bytes, or NULL; never NULL for n == 0 without
 * an error, as Go's slice is not nil. */
static const uint8_t *take(Decoder *d, int64_t n) {
	if (d->err)
		return NULL;
	if (n < 0 || (uint64_t)n > d->n - d->pos) {
		short_(d);
		return NULL;
	}
	d->pos += (size_t)n;
	return d->b + d->pos - n;
}

static uint64_t u64(Decoder *d) {
	const uint8_t *s = take(d, 8);
	if (s == NULL)
		return 0;
	uint64_t x = 0;
	for (int i = 7; i >= 0; i--)
		x = x << 8 | s[i];
	return x;
}

static Str str(Decoder *d) {
	int32_t n = small(d);
	const uint8_t *s = take(d, n);
	return s ? (Str){(const char *)s, (size_t)n} : (Str){"", 0};
}

static Str *strs(Decoder *d, int *np) {
	int n = count(d);
	*np = n;
	if (n == 0)
		return NULL;
	Str *s = xcalloc((size_t)n, sizeof *s);
	for (int i = 0; i < n; i++)
		s[i] = str(d);
	return s;
}

static bool streq(Str s, const char *t) { return s.n == strlen(t) && memcmp(s.s, t, s.n) == 0; }

static bool inlist(Str s, const char *const *list, int n) {
	for (int i = 0; i < n; i++)
		if (streq(s, list[i]))
			return true;
	return false;
}

/* resolve reports whether r names a builtin this VM knows. */
static bool resolve(const BuiltinRef *r) {
	switch (r->kind) {
	case BuiltinOp:
		return inlist(r->name, gen_ops, gen_nops);
	case BuiltinFunction:
		return inlist(r->name, gen_functions, gen_nfunctions);
	case BuiltinAsgn:
	case BuiltinField:
	case BuiltinUnsupported:
		return true;
	case BuiltinApply:
		return r->name.n == 0;
	}
	return false;
}

static void builtins_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->builtins = xcalloc((size_t)n, sizeof *p->builtins);
	for (int i = 0; i < n && !d->err; i++) {
		BuiltinRef r;
		r.kind = byte(d);
		r.name = str(d);
		if (d->err)
			return;
		if (!resolve(&r)) {
			d->err = msg("unknown builtin ");
			builtinref(d->err, &r);
			bufputs(d->err, " (built by a newer Gcon?)");
			return;
		}
		p->builtins[p->nbuiltins++] = r;
	}
}

static void records_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->records = xcalloc((size_t)n, sizeof *p->records);
	for (int i = 0; i < n && !d->err; i++) {
		RecordType *r = &p->records[p->nrecords++];
		r->name = str(d);
		r->fields = strs(d, &r->nfields);
	}
}

static Operand operand(Decoder *d) {
	uint64_t x = uvarint(d);
	int k = (int)(x & 7);
	if (k == NoOperand || k > ConstOp)
		fail(d, "bad operand kind %d", k);
	if (x >> 3 > INT32_MAX)
		fail(d, "operand index out of range");
	return (Operand){(uint8_t)k, (int32_t)(x >> 3)};
}

static void proc(Decoder *d, Proc *p) {
	p->name = str(d);
	p->file = str(d);
	p->nparams = small(d);
	switch (byte(d)) {
	case 0:
		break;
	case 1:
		p->variadic = true;
		break;
	default:
		fail(d, "bad variadic flag");
	}
	p->nlocals = small(d);
	p->localnames = strs(d, &p->nlocalnames);
	int n = count(d);
	if (n > 0) {
		p->statics = xcalloc((size_t)n, sizeof *p->statics);
		p->nstatics = n;
		for (int i = 0; i < n; i++)
			p->statics[i] = small(d);
	}
	p->files = strs(d, &p->nfiles);
	p->ntemps = small(d);
	n = count(d);
	p->code = xcalloc((size_t)n, sizeof *p->code);
	p->ncode = n;
	int64_t line = 0;
	for (int i = 0; i < n && !d->err; i++) {
		Instr *in = &p->code[i];
		in->fn = -1;
		in->op = byte(d);
		uint8_t flags = byte(d);
		line = (int64_t)((uint64_t)line + (uint64_t)varint(d)); /* wraps, as Go's */
		if (line < INT32_MIN || line > INT32_MAX)
			fail(d, "line out of range");
		in->line = (int32_t)(uint32_t)(uint64_t)line;
		if (flags & InstrDst)
			in->dst = operand(d);
		if (flags & InstrX)
			in->x = operand(d);
		if (flags & InstrH)
			in->h = small(d);
		if (flags & InstrArgs) {
			in->nargs = count(d);
			in->args = xcalloc((size_t)in->nargs, sizeof *in->args);
			for (int j = 0; j < in->nargs; j++)
				in->args[j] = operand(d);
		}
		if (flags & InstrFn) {
			int32_t j = small(d);
			if (j >= d->p->nbuiltins) {
				fail(d, "builtin %d out of range", j);
				return;
			}
			in->fn = j;
		}
		if (flags & InstrL1)
			in->l1 = small(d);
		if (flags & InstrL2)
			in->l2 = small(d);
		if (flags & InstrFile)
			in->file = small(d);
	}
}

static void procs_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->procs = xcalloc((size_t)n, sizeof *p->procs);
	for (int i = 0; i < n && !d->err; i++)
		proc(d, &p->procs[p->nprocs++]);
}

/* integer returns the integer with sign neg and magnitude mag (n bytes,
 * big-endian): a small integer if it fits in 64 bits, as gcon's
 * mkInteger, else a large one. */
static Value integer(bool neg, const uint8_t *mag, size_t n) {
	while (n > 0 && mag[0] == 0)
		mag++, n--;
	if (n <= 8) {
		uint64_t m = 0;
		for (size_t i = 0; i < n; i++)
			m = m << 8 | mag[i];
		if (!neg && m <= INT64_MAX)
			return (Value){MKH(KInt, 0, 0), {.i = (int64_t)m}};
		if (neg && m <= (uint64_t)INT64_MAX + 1)
			return (Value){MKH(KInt, 0, 0), {.i = (int64_t)(0 - m)}};
	}
	int nd = (int)((n + 3) / 4);
	Big *b = xcalloc(1, sizeof *b + (size_t)nd * sizeof b->d[0]);
	b->neg = neg;
	b->n = nd;
	for (size_t i = 0; i < n; i++) {
		size_t k = n - 1 - i; /* byte index from the least significant */
		b->d[k / 4] |= (uint32_t)mag[i] << (8 * (k % 4));
	}
	return (Value){MKH(KBig, 0, 0), {.p = b}};
}

static Value value(Decoder *d) {
	Program *p = d->p;
	uint8_t t = byte(d);
	switch (t) {
	case ValNull:
		return (Value){MKH(KNull, 0, 0), {0}};
	case ValInt:
		return (Value){MKH(KInt, 0, 0), {.i = varint(d)}};
	case ValBig: {
		uint8_t neg = byte(d);
		int32_t n = small(d);
		const uint8_t *mag = take(d, n);
		if (neg != 0 && neg != 1)
			fail(d, "bad sign");
		if (mag == NULL)
			n = 0;
		return integer(neg == 1, mag, (size_t)n);
	}
	case ValReal: {
		uint64_t u = u64(d);
		double r;
		memcpy(&r, &u, sizeof r);
		return (Value){MKH(KReal, 0, 0), {.r = r}};
	}
	case ValString: {
		Str s = str(d);
		return (Value){MKH(KString, 0, s.n), {.s = s.s}};
	}
	case ValCset: {
		Cset *c = xcalloc(1, sizeof *c);
		for (int i = 0; i < 4; i++)
			c->bits[i] = u64(d);
		return (Value){MKH(KCset, 0, 0), {.p = c}};
	}
	case ValUset: {
		Uset *c = xcalloc(1, sizeof *c);
		for (int i = 0; i < 4; i++)
			c->bits[i] = u64(d);
		int n = count(d);
		if (n > 0) {
			c->ranges = xcalloc((size_t)n, sizeof *c->ranges);
			uint64_t prev = 0x7F;
			for (int i = 0; i < n; i++) {
				uint64_t lo = uvarint(d), hi = uvarint(d);
				if (lo > hi || hi > MaxRune || lo <= prev + (prev >= 0x80) ||
				    (lo <= SurrHi && hi >= SurrLo)) {
					fail(d, "bad uset range");
					break;
				}
				c->ranges[i][0] = (uint32_t)lo;
				c->ranges[i][1] = (uint32_t)hi;
				c->nranges++;
				prev = hi;
			}
		}
		return (Value){MKH(KUset, 0, 0), {.p = c}};
	}
	case ValBuiltin: {
		int32_t i = small(d);
		if (i < p->nbuiltins)
			return (Value){MKH(KBuiltin, 0, i), {0}};
		fail(d, "builtin out of range");
		break;
	}
	case ValProc: {
		int32_t i = small(d);
		if (i < p->nprocs)
			return (Value){MKH(KProc, 0, 0), {.p = &p->procs[i]}};
		fail(d, "procedure out of range");
		break;
	}
	case ValRecord: {
		int32_t i = small(d);
		if (i < p->nrecords)
			return (Value){MKH(KRecordType, 0, 0), {.p = &p->records[i]}};
		fail(d, "record type out of range");
		break;
	}
	default:
		fail(d, "bad value tag %d", t);
	}
	return (Value){MKH(KNull, 0, 0), {0}};
}

static void consts_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->consts = xcalloc((size_t)n, sizeof *p->consts);
	for (int i = 0; i < n; i++)
		p->consts[p->nconsts++] = value(d);
}

static void globals_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->globals = xcalloc((size_t)n, sizeof *p->globals);
	p->globalnames = xcalloc((size_t)n, sizeof *p->globalnames);
	for (int i = 0; i < n && !d->err; i++) {
		p->globalnames[i] = str(d);
		p->globals[i] = value(d);
		p->nglobals++;
	}
	p->main = varint(d);
	switch (byte(d)) {
	case 0:
		break;
	case 1: {
		const uint8_t *bits = take(d, (n + 7) / 8);
		if (bits) {
			p->invocable = xcalloc((size_t)n, sizeof *p->invocable);
			for (int i = 0; i < n; i++)
				p->invocable[i] = bits[i / 8] & (1 << (i % 8));
		}
		break;
	}
	default:
		fail(d, "bad invocable flag");
	}
}

/* addinfo records key's value, replacing any earlier one. */
static void addinfo(Program *p, Str k, Str v) {
	for (int i = 0; i < p->ninfo; i++)
		if (p->infokeys[i].n == k.n && memcmp(p->infokeys[i].s, k.s, k.n) == 0) {
			p->infovals[i] = v;
			return;
		}
	p->infokeys[p->ninfo] = k;
	p->infovals[p->ninfo++] = v;
}

static int strcmp_(Str a, Str b) {
	int c = memcmp(a.s, b.s, a.n < b.n ? a.n : b.n);
	return c ? c : (a.n > b.n) - (a.n < b.n);
}

static void info_section(Decoder *d) {
	Program *p = d->p;
	int n = count(d);
	p->ninfo = 0; /* a later info section replaces an earlier one */
	p->infokeys = xcalloc((size_t)n, sizeof *p->infokeys);
	p->infovals = xcalloc((size_t)n, sizeof *p->infovals);
	for (int i = 0; i < n; i++) {
		Str k = str(d);
		addinfo(p, k, str(d));
	}
	for (int i = 1; i < p->ninfo; i++)
		for (int j = i; j > 0 && strcmp_(p->infokeys[j - 1], p->infokeys[j]) > 0; j--) {
			Str t = p->infokeys[j];
			p->infokeys[j] = p->infokeys[j - 1];
			p->infokeys[j - 1] = t;
			t = p->infovals[j];
			p->infovals[j] = p->infovals[j - 1];
			p->infovals[j - 1] = t;
		}
}

/* quoted returns tag as Go's %q writes it, NUL-terminated (its escapes
 * leave no NUL within). */
static char *quoted(const uint8_t *tag) {
	Buf b = {0};
	goquote(&b, (const char *)tag, 4);
	bufputc(&b, 0);
	return b.b;
}

/* finish checks that a section decoded without error and was consumed. */
static Buf *finish(Decoder *s, const uint8_t *tag) {
	char *q = quoted(tag);
	Buf *e = NULL;
	if (s->err)
		e = wrap(msg("section %s: ", q), s->err);
	else if (s->pos != s->n)
		e = msg("section %s: %zu bytes left over", q, s->n - s->pos);
	free(q);
	return e;
}

static const char *const required[] = {"Bltn", "Recd", "Proc", "Cnst", "Glob"};
enum { NRequired = 5 };

/* sections decodes the sections that follow the version. */
static Buf *sections(Decoder *d) {
	int next = 0; /* index in required of the next required section */
	for (;;) {
		size_t start = d->pos;
		const uint8_t *tag = take(d, 4);
		int32_t n = small(d);
		const uint8_t *body = take(d, n);
		if (d->err)
			return d->err;
		Decoder s = {body, (size_t)n, 0, NULL, d->p};
		if (memcmp(tag, "End ", 4) == 0) {
			if (next < NRequired)
				return msg("missing section \"%s\"", required[next]);
			if (n != 4)
				return msg("bad \"End \" section");
			uint32_t crc = (uint32_t)body[0] | (uint32_t)body[1] << 8 |
				       (uint32_t)body[2] << 16 | (uint32_t)body[3] << 24;
			if (crc32_ieee(d->b, start) != crc)
				return msg("checksum mismatch");
			if (d->pos != d->n)
				return msg("data after the end");
			return NULL;
		}
		if (tag[0] >= 'a' && tag[0] <= 'z') {
			if (memcmp(tag, "info", 4) == 0) {
				info_section(&s);
				Buf *e = finish(&s, tag);
				if (e)
					return e;
			}
			continue; /* a section this gcon-vm need not understand */
		}
		if (next >= NRequired || memcmp(tag, required[next], 4) != 0) {
			char *q = quoted(tag);
			Buf *e = NULL;
			for (int i = 0; i < NRequired; i++)
				if (memcmp(tag, required[i], 4) == 0)
					e = msg("section %s out of order", q);
			if (!e)
				e = msg("unknown section %s (built by a newer Gcon?)", q);
			free(q);
			return e;
		}
		switch (next++) {
		case 0:
			builtins_section(&s);
			break;
		case 1:
			records_section(&s);
			break;
		case 2:
			procs_section(&s);
			break;
		case 3:
			consts_section(&s);
			break;
		case 4:
			globals_section(&s);
			break;
		}
		Buf *e = finish(&s, tag);
		if (e)
			return e;
	}
}

static bool inrange(const Program *p, const Proc *pr, Operand o) {
	int64_t i = o.index;
	switch (o.kind) {
	case NoOperand:
		return i == 0;
	case TempOp:
		return i < pr->ntemps;
	case LocalOp:
		return i < pr->nlocals;
	case GlobalOp:
		return i < p->nglobals;
	case ConstOp:
		return i < p->nconsts;
	}
	return false;
}

static Buf *validate_proc(const Program *p, const Proc *pr) {
	if (pr->nparams > pr->nlocals || pr->nlocalnames > pr->nlocals)
		return msg("bad locals");
	if (pr->variadic && pr->nparams == 0)
		return msg("variadic with no parameters");
	for (int i = 0; i < pr->nstatics; i++)
		if (pr->statics[i] >= p->nglobals)
			return msg("static %d out of range", pr->statics[i]);
	if (pr->ncode == 0)
		return msg("no code");
	for (int pc = 0; pc < pr->ncode; pc++) {
		const Instr *in = &pr->code[pc];
		const char *bad = NULL;
		if (in->op >= NumOps)
			return msg("instruction %d: bad opcode %d", pc, in->op);
		if (!inrange(p, pr, in->dst))
			bad = "destination";
		else if (!inrange(p, pr, in->x))
			bad = "operand";
		for (int j = 0; !bad && j < in->nargs; j++)
			if (!inrange(p, pr, in->args[j]) || in->args[j].kind == NoOperand)
				bad = "argument";
		if (!bad && (in->l1 < 0 || in->l1 >= pr->ncode || in->l2 < 0 || in->l2 >= pr->ncode))
			bad = "label";
		if (!bad && (in->file < 0 || in->file > pr->nfiles))
			bad = "file";
		if (!bad && (in->h < 0 || in->h >= (pr->ntemps > 1 ? pr->ntemps : 1)))
			bad = "handle";
		/* The kinds the VM relies on. */
		if (!bad)
			switch (in->op) {
			case OpSetLabel: case OpMove: case OpMoveVar: case OpOper: case OpCall:
			case OpResume: case OpCreate: case OpActivate: case OpScanEnter:
				if (in->dst.kind != TempOp)
					bad = "destination";
				break;
			case OpAssign:
				if (in->dst.kind == NoOperand)
					bad = "destination";
				break;
			}
		if (!bad)
			switch (in->op) {
			case OpIGoto: case OpScanSwap:
				if (in->x.kind != TempOp)
					bad = "operand";
				break;
			case OpOnce:
				if (in->x.kind != GlobalOp)
					bad = "operand";
				break;
			case OpMove: case OpMoveVar: case OpAssign: case OpCall: case OpReturn:
			case OpSuspend: case OpIsNull: case OpActivate: case OpCoRet: case OpScanEnter:
				if (in->x.kind == NoOperand)
					bad = "operand";
				break;
			}
		if (!bad)
			switch (in->op) {
			case OpOper:
				if (in->fn < 0)
					bad = "function";
				break;
			case OpCall: case OpResume:
				if (pr->ntemps == 0)
					bad = "handle";
				break;
			}
		if (bad)
			return msg("instruction %d (%s): bad %s", pc, opnames[in->op], bad);
	}
	return NULL;
}

/* validate checks that the indices in p are in range and that each
 * instruction's operands are of the kinds the VM assumes. As gcon's, it
 * checks the procedures the globals hold. */
static Buf *validate(const Program *p) {
	if (p->main != -1) {
		if (p->main < 0 || p->main >= p->nglobals)
			return msg("main %lld out of range", (long long)p->main);
		if (VKIND(p->globals[p->main]) != KProc)
			return msg("main is not a procedure");
	}
	bool *seen = xcalloc((size_t)p->nprocs, sizeof *seen);
	Buf *e = NULL;
	for (int i = 0; i < p->nglobals && !e; i++) {
		if (VKIND(p->globals[i]) != KProc)
			continue;
		const Proc *pr = p->globals[i].w.p;
		if (seen[pr - p->procs])
			continue;
		seen[pr - p->procs] = true;
		Buf *pe = validate_proc(p, pr);
		if (pe) {
			e = msg("procedure ");
			bufstr(e, pr->name);
			bufputs(e, ": ");
			e = wrap(e, pe);
		}
	}
	free(seen);
	return e;
}

static Buf *damaged(Buf *e) { return wrap(msg("damaged program: "), e); }

/* decode decodes the encoded program b, which must stay in place while
 * the program is used: its strings point into it. It returns NULL and
 * the program in *pp, or a message, which may hold any bytes. */
Buf *decode(const uint8_t *b, size_t n, Program **pp) {
	if (n < sizeof Magic || memcmp(b, Magic, sizeof Magic) != 0)
		return msg("not a gcon program");
	Program *p = xcalloc(1, sizeof *p);
	Decoder d = {b, n, sizeof Magic, NULL, p};
	uint64_t v = uvarint(&d);
	if (d.err)
		return damaged(d.err);
	if (v != FormatVersion)
		return msg("program built by %s Gcon (format %llu; this Gcon reads format %d): rebuild it",
			   v > FormatVersion ? "a newer" : "an older", (unsigned long long)v, FormatVersion);
	Buf *e = sections(&d);
	if (!e)
		e = validate(p);
	if (e)
		return damaged(e);
	*pp = p;
	return NULL;
}
