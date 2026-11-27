/*
 * gcon-vm: the virtual machine for gcon's bytecode (docs/design.md).
 * Declarations shared by the sources.
 */
#ifndef GCON_VM_H
#define GCON_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "format.h"

#define GCONVM_VERSION "0.0.0"

/* util.c */

/* Str is a run of bytes, not NUL-terminated. */
typedef struct Str {
	const char *s;
	size_t n;
} Str;

/* Buf is a growable byte buffer. */
typedef struct Buf {
	char *b;
	size_t n, cap;
} Buf;

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
void bufadd(Buf *b, const void *p, size_t n);
void bufputc(Buf *b, int c);
void bufputs(Buf *b, const char *s);
void bufstr(Buf *b, Str s);
void bufprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint32_t crc32_ieee(const uint8_t *p, size_t n);

/* UTF-8, as Go's unicode/utf8. */
enum { RuneError = 0xFFFD, RuneSelf = 0x80, MaxRune = 0x10FFFF, SurrLo = 0xD800, SurrHi = 0xDFFF };
int32_t utf8decode(const uint8_t *p, size_t n, int *width);
int utf8encode(uint8_t *p, int32_t r);
bool isprint_rune(int32_t r); /* Go's strconv.IsPrint */

/* Values: 16 bytes, a header (kind, flags, a 48-bit field) and a word
 * (docs/design.md, "Values"). Kinds as gcon's valuecore.go. */
enum Kind {
	KNone, KNull, KInt, KReal, KBig, KString, KCset, KList, KTable, KSet,
	KRecord, KProc, KBuiltin, KRecordType, KCoexp, KFile,
	KRef, KListElem, KField, KSubstr, KVar,
	KFrame, KGen, KScanEnv, KScanEnvBox,
	KExternal, /* a C function's external value (cfunc.c): gcon-vm only */
};

typedef struct Value {
	uint64_t h;
	union {
		int64_t i;
		double r;
		void *p;
		const char *s;
	} w;
} Value;

#define VKIND(v) ((int)((v).h & 0xff))
#define VFLAGS(v) ((int)(((v).h >> 8) & 0xff))
#define VN(v) ((v).h >> 16)
#define MKH(k, f, n) ((uint64_t)(k) | (uint64_t)(f) << 8 | (uint64_t)(n) << 16)

/* A string's flags: its unit. */
enum { SBytes = 1 };

/* Cset: the ASCII characters and the non-ASCII bytes in bits, code
 * points from 0x80 in ranges (sorted, disjoint, not adjacent), as
 * gcon's cset.go. */
typedef struct Cset {
	uint64_t bits[4];
	int nranges;
	uint32_t (*ranges)[2];
	void *str; /* run time: the cset as a string, once made (a cset never changes) */
} Cset;

/* Big: a large integer, sign and magnitude in base 2^32 digits, least
 * significant first, with no leading zero digits. */
typedef struct Big {
	bool neg;
	int n;
	uint32_t d[];
} Big;

/* Programs, as gcon's internal/vm/instr.go and value.go. */

typedef struct Operand {
	uint8_t kind;
	int32_t index;
} Operand;

extern const char *const opnames[NumOps];

typedef struct Instr {
	uint8_t op;
	Operand dst, x;
	int32_t h;
	int32_t nargs;
	Operand *args;
	int32_t fn; /* index in Program.builtins, or -1 */
	void *fnp; /* the Builtin, once linked */
	int32_t l1, l2;
	int32_t line;
	int32_t file; /* 0: the procedure's file; k: its files[k-1] */
} Instr;

typedef struct Proc {
	Str name, file;
	int nparams;
	bool variadic;
	int nlocals;
	int nlocalnames;
	Str *localnames;
	int nstatics;
	int *statics;
	int nfiles;
	Str *files;
	int ntemps;
	int ncode;
	Instr *code;
} Proc;

typedef struct RecordType {
	Str name;
	int nfields;
	Str *fields;
	int64_t serial; /* the last serial number issued (run time) */
} RecordType;

typedef struct BuiltinRef {
	uint8_t kind;
	Str name;
} BuiltinRef;

typedef struct Program {
	int nbuiltins;
	BuiltinRef *builtins;
	int nrecords;
	RecordType *records;
	int nprocs;
	Proc *procs;
	int nconsts;
	Value *consts;
	int nglobals;
	Value *globals;
	Str *globalnames;
	int64_t main; /* global index of main, or -1 */
	bool *invocable; /* by global; NULL: all */
	int ninfo;
	Str *infokeys, *infovals; /* the build information, sorted by key */
} Program;

/* load.c */
extern const char Magic[8];
const uint8_t *encoded(const uint8_t *data, size_t n, size_t *len);
Buf *decode(const uint8_t *b, size_t n, Program **pp);

/* image.c */
void builtinref(Buf *b, const BuiltinRef *r);
void goquote(Buf *b, const char *s, size_t n);
void quote(Buf *b, const char *s, size_t n, bool bytes, char q);
void imagecset(Buf *b, const Cset *c);
void rtos(Buf *b, double f);
void bigimage(Buf *b, const Big *x);
void constimage(Buf *b, const Program *p, Value v);

/* dis.c */
void disasm(Buf *b, const Program *p);

/* gen_tables.c */
extern const char *const gen_ops[];
extern const int gen_nops;
extern const char *const gen_functions[];
extern const int gen_nfunctions;
extern const uint32_t gen_nonprint[][2];
extern const int gen_nnonprint;

#endif
