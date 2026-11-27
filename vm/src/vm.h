/*
 * The run-time system: values, structures, builtins and the interpreter,
 * ported from gcon's Go VM (gcon's internal/vm), whose semantics it
 * follows exactly; the Go VM is the reference (docs/design.md).
 *
 * Memory is managed by the collector (gc.c): objects are made with gcnew.
 */
#ifndef GCON_VM_RT_H
#define GCON_VM_RT_H

#include "gcon-vm.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* ---- Strings ---- */

/* String is an Icon string: its bytes (not NUL-terminated, shared, never
 * changed) and its length. */
typedef struct String {
	const char *s;
	int64_t n;
} String;

String mkstr(const char *s, int64_t n);
String textc(const char *s); /* a C string */
String strcopy(const char *s, int64_t n);

/* SB builds a string in the heap: its buffer becomes the string, uncopied.
 * Safe because the collector runs only between instructions. */
typedef struct SB {
	char *b;
	size_t n, cap;
} SB;
void sbgrow_(SB *s, size_t k);
static inline void sbgrow(SB *s, size_t k) {
	if (s->cap - s->n < k)
		sbgrow_(s, k);
}
static inline void sbputc(SB *s, int c) {
	sbgrow(s, 1);
	s->b[s->n++] = (char)c;
}
static inline void sbadd(SB *s, const void *p, size_t k) {
	sbgrow(s, k);
	memcpy(s->b + s->n, p, k);
	s->n += k;
}
String sbstring(SB *s);
String slice(String s, int64_t i, int64_t j); /* 1-based, i <= j */
String concat(String a, String b);
bool streq_(String a, const char *t);
char *cstr(String s); /* a NUL-terminated copy */

/* A string Value: n is the length, w.s the bytes. */
Value strval(String s);
String vstr(Value v);

/* ---- Values ---- */

#define NONE ((Value){0, {0}})
#define NULLV ((Value){MKH(KNull, 0, 0), {0}})
static inline Value intval(int64_t i) { return (Value){MKH(KInt, 0, 0), {.i = i}}; }
static inline Value realval(double r) { return (Value){MKH(KReal, 0, 0), {.r = r}}; }
static inline Value ptrval(int k, void *p) { return (Value){MKH(k, 0, 0), {.p = p}}; }
static inline Value idxval(int k, void *p, int64_t i) { return (Value){MKH(k, 0, i), {.p = p}}; }
static inline bool isvar(Value v) { return VKIND(v) >= KRef && VKIND(v) <= KVar; }

/* ---- Structures ---- */

typedef struct List {
	Value *e;   /* the elements are e[off .. off+n) */
	int64_t n, off, cap;
	int64_t serial;
} List;

typedef struct Record {
	RecordType *t;
	int64_t serial;
	Value f[];
} Record;

/* An insertion-ordered map keyed by ===, for tables and sets. */
typedef struct OMap {
	Value *keys, *vals; /* by insertion; a deleted entry's key is NONE */
	int64_t n, cap;     /* entries used, including deleted ones */
	int64_t live, dead;
	int32_t *index;     /* open addressing: entry + 1, or 0 */
	int64_t isize;      /* a power of two */
	bool shared;        /* a traversal holds keys */
} OMap;

typedef struct Table {
	OMap m;
	Value dflt;
	int64_t serial;
} Table;

typedef struct Set {
	OMap m;
	int64_t serial;
} Set;

/* A substring variable, as iconx's substring trapped variable. */
typedef struct Substr {
	Value v;          /* the variable: a KRef or a variable other than KSubstr */
	int64_t pos, n;   /* 1-based byte position and length */
} Substr;

/* KVar: the variables a Value cannot hold directly. */
enum VarKind {
	VSubject, VPos, VRandom, VTrace, VError, VProgname, VDump, VValue, VTableElem,
	VGraphics, /* a graphics keyword (graphics.c): k its index */
};
typedef struct Var {
	int kind;
	Value *p;      /* VValue */
	Table *t;      /* VTableElem */
	Value k;
} Var;

typedef struct ScanEnv {
	String subject;
	int64_t pos;
} ScanEnv;

/* ---- Files ---- */

typedef struct File {
	String name;
	bool keyword; /* a standard file: its image is its name */
	void *r, *w;  /* buffers for reading, writing (either may be NULL) */
	int fd;       /* the file opened, for seek, where and close; -1 for streams */
	bool pipe;
	bool untrans; /* mode "u": only "\n" ends a line, as iconx's Fs_Untrans */
	bool closed;
	bool errout;
	bool isdir;
	char **dir; /* a directory's entries not yet read */
	int ndir, idir;
	int peeked; /* a byte read ahead (kbhit, \r\n), or -1 */
	void *gfx;  /* a window: its binding (graphics.c) */
} File;

/* ---- Generators ---- */

typedef struct Gen Gen;
struct Gen {
	bool (*next)(Gen *g, Value *out);
	void (*mark)(Gen *g); /* marks what it refers to (gc.c); NULL if nothing */
};

/* ---- The collector (gc.c) ---- */

/* The types of heap blocks: how the collector traces them. */
enum {
	T_RAW,     /* no pointers: string bytes, digits, ranges */
	T_VALUES,  /* an array of Values */
	T_LIST, T_RECORD, T_TABLE, T_SET, T_SUBSTR, T_VAR, T_SCANENV,
	T_GEN,     /* begins with a Gen: its mark function traces it */
	T_FRAME, T_COEXP, T_CSET, T_USET,
	T_STRHDR,  /* a String */
	T_FILE,
	T_GCVOID,  /* an array of pointers to heap objects */
	T_STRINGS, /* an array of Strings */
};

void *gcnew(size_t n, int type);
void *gcraw(size_t n); /* T_RAW, not zeroed: for bytes about to be written */
void gc_allocated(int64_t out[4]); /* &allocated: total, static, string, block */
void gc_unallocated(const int64_t snap[4]); /* back to a snapshot */
void *gcalloc(size_t n, int type, bool zero); /* not zeroed: the caller initialises it all */
bool markptr(const void *p);
void markvalue(Value v);
void gcroot(void **p);
void gc_collect(void);
void gc_init(void);
extern bool gc_pending;

/* ---- Builtins ---- */

typedef struct Builtin Builtin;
typedef bool (*FnFn)(Builtin *b, Value *a, int n, Value *out);
typedef Gen *(*GenFn)(Builtin *b, Value *a, int n);
typedef bool (*RestoreFn)(Builtin *b, Value *a, int n, Value *out, int64_t *old);

/* How a call written in the program adjusts a builtin's arguments. */
enum { AdjNone, AdjMin, AdjExact };

/* Fast paths the interpreter computes itself. */
enum {
	IntNone, IntAdd, IntSub, IntMul, IntLT, IntLE, IntGT, IntGE, IntEQ, IntNE,
	OpSize, OpPlus, OpNeg, OpIdent, OpNotIdent, OpIndex, OpField
};

struct Builtin {
	const char *name; /* as iconx names it: "-" for u-, ".f" for a field */
	const char *key;  /* the key in its table ("u-", "write") */
	uint8_t table;    /* BuiltinOp, BuiltinFunction, ... */
	FnFn fn;
	GenFn gen;
	RestoreFn restore;
	bool noderef, reffirst;
	uint8_t intop;
	uint8_t adjmode;
	int adjn;
	int arity;     /* for args(): funcArity, or -1 */
	Str field;     /* a field operator's field */
	int *fidx;     /* its index in each record type, by type number (-2: not yet looked up) */
	bool swap;     /* an assignment operator that exchanges: :=: or <-> */
	void *cfunc;   /* a function loadfunc loaded: its C entry point */
};

void builtins_init(void);
Builtin *findop(const char *key);
Builtin *findfn(const char *name, size_t n);
Builtin *fieldop(Str name);
int fieldindex(Builtin *b, RecordType *t); /* b's field in t, or -1 */
Builtin *asgnop(Str name);
Builtin *unsupported(Str name);
extern Builtin applyop;
void reg(Builtin *b, int table); /* register a builtin in Ops or Functions */
int nfunctions(void);
Builtin *function_at(int i); /* sorted by name */

/* ---- The interpreter ---- */

typedef struct Frame Frame;
struct Frame {
	Proc *proc;
	Value *locals;
	Value *temps;
	int pc;
	int suspended; /* it has suspended: its caller may hold it, so it is never recycled */
	Frame *caller;
	Value slots[];
};

enum CoState { CoNew, CoRunning, CoResult, CoWaiting, CoDone };

typedef struct Coexp {
	int64_t serial;
	Proc *proc;
	int entry;
	Value *init; /* the creator's locals */
	Frame *frame;
	struct Coexp *activator;
	int state;
	int64_t results;
	char *from;
	int64_t stack;
} Coexp;

typedef struct VM {
	Program *prog;
	Builtin **builtins; /* the program's builtins, by index */
	Value *globals;
	Frame *cur;
	Coexp *co, *main;
	String subject;
	int64_t pos;
	File *input, *output, *errfile;
	File **files;
	int nfiles, capfiles;
	Buf partial;
	File *partialfile;
	bool haspartial;
	int64_t stacklimit;
	/* error conversion */
	int64_t errorkw;
	int errnum;
	Value errval;
	bool haveerrval;
	/* the error being raised */
	int rerrnum;
	Value rerrval;
	bool rerrhas, rerrfatal;
	jmp_buf jmp;
	int64_t random;
	int64_t lastcharpos;
	int64_t trace;
	int64_t level;
	int64_t dump;
	int64_t collections[4];
	String progname;
	Value eventcode, eventsource, eventvalue;
	int64_t serial_lists, serial_tables, serial_sets, serial_coexps;
	Value *scratch;
	int nscratch;
	/* map's last translation table */
	void *maptab;
	/* The generator a call of a builtin generator replaces: its handle's
	 * slot, the only reference to it, is overwritten by the call, so it is
	 * dead and newgen may reuse it. */
	Gen *spare;
} VM;

extern VM vm;

/* newgen makes a builtin generator of the given size and next function,
 * reusing the one the call replaces if it is of the same kind. The
 * caller sets every field. */
static inline void *newgen(size_t size, bool (*next)(Gen *, Value *)) {
	Gen *s = vm.spare;
	if (s && s->next == next) {
		vm.spare = NULL;
		return s;
	}
	return gcnew(size, T_GEN);
}

_Noreturn void runerr(int num);
_Noreturn void runerrv(int num, Value v);
_Noreturn void runerr_fatal(int num);
_Noreturn void vm_exit(int code);
extern const char *const errortext[];
const char *errtext(int num);

int run_program(Program *p, const char *progname, int argc, char **argv);
void link_program(Program *p);

/* ---- Conversions and helpers (conv.c) ---- */

#define ARG(i) ((i) < n ? a[i] : NULLV)
#define ISNULL(v) (VKIND(v) == KNull)

Value derefvar(Value v);
/* deref: the value of v if it is a variable, else v itself (the usual
 * case, kept inline; a local's KRef too). */
static inline Value deref(Value v) {
	if (!isvar(v))
		return v;
	if (VKIND(v) == KRef)
		return *(Value *)v.w.p;
	return derefvar(v);
}
bool assignvar(Value dst, Value x, bool *isvar_);
bool setvar(Value ref, Value v);
bool cvnum(Value v, Value *out);
bool cvint(Value v, int64_t *out);
bool cvinteger(Value v, Value *out);
bool cvreal(Value v, double *out);
bool cvstr(Value v, String *out);
bool cvcset(Value v, Cset **out);
int64_t mustint(Value v);
Value mustinteger(Value v);
Value mustnum(Value v);
double mustreal(Value v);
String muststr(Value v);
Cset *mustcset(Value v);
List *mustlist(Value v);
bool cvpos(int64_t p, int64_t n, int64_t *out);
bool str2num(String s, Value *out);
bool identical(Value x, Value y);
const char *typename_(Value v);
double tofloat(Value n);
bool realtoint(double f, int64_t *out);
bool isinteger(Value v);
int cmpinteger(Value x, Value y);
void itoa_(Buf *b, int64_t i);

/* ---- Numbers (num.c, big.c) ---- */

void num_init(void);
Big *bigofint(int64_t i);
Value mkinteger(Big *b); /* normalises: a small integer if it fits */
void bigstr(Buf *b, const Big *x);
bool bigtoreal(const Big *x, double *out);
bool realtointeger(double f, Value *out);
Value bigadd(Value x, Value y);
Value bigsub(Value x, Value y);
Value bigmul(Value x, Value y);
Value bigdiv(Value x, Value y);
Value bigmod(Value x, Value y);
Value bigneg(Value x);
int bigcmp(Value x, Value y);
Value bigpow(Value x, int64_t n);
Value bigand(Value x, Value y);
Value bigor(Value x, Value y);
Value bigxor(Value x, Value y);
Value bignot(Value x);
Value bigshift(Value x, int64_t n);
bool bigparse(const char *s, size_t n, int base, Value *out);
bool bigbit0(Value x);
int bigsign(Value x);
Big *bigval(Value x);
Value bigrand(Value x);
double realpowbig(double a, Value b);
bool addint(int64_t x, int64_t y, int64_t *r);
bool subint(int64_t x, int64_t y, int64_t *r);
bool mulint(int64_t x, int64_t y, int64_t *r);
Value neg_(Value x);

/* ---- Csets (cset.c) ---- */

Cset *newcset(void);
Cset *csetof(String s);
Cset *csetfrom(const char *s, int64_t n);
Cset *csetop(int op, const Cset *a, const Cset *b);
Cset *complement(const Cset *c);
static inline bool cset_has(const Cset *c, int b) { return c->bits[b >> 6] >> (b & 63) & 1; }
static inline void cset_set(Cset *c, int b) { c->bits[b >> 6] |= (uint64_t)1 << (b & 63); }
int64_t cset_size(const Cset *c);
bool cset_equal(const Cset *a, const Cset *b);
String cset_string(const Cset *c);
extern Cset *CsetAll, *CsetASCII, *CsetDigits, *CsetLcase, *CsetUcase, *CsetLetters;
void cset_init(void);
const char *csetname_(const Cset *c);

/* ---- Usets (uset.c) ---- */

Uset *newuset(void);
Uset *usetof(String s);           /* the characters of s read as UTF-8 */
Uset *usetofcset(const Cset *c);
Uset *usetop(int op, const Uset *a, const Uset *b);
Uset *ucomplement(const Uset *c);
static inline bool uset_hasbyte(const Uset *c, int b) { return c->bits[b >> 6] >> (b & 63) & 1; }
bool uset_hasrune(const Uset *c, int32_t r);
int64_t uset_size(const Uset *c);
bool uset_equal(const Uset *a, const Uset *b);
bool uset_isuniv(const Uset *c);
String uset_string(const Uset *c);
bool umatch(const Uset *c, String s, int64_t off, int *w);
typedef struct UsetIter {
	int next, ri;
	uint32_t x;
} UsetIter;
bool uset_next(const Uset *c, UsetIter *it, String *out);
String uset_nth(const Uset *c, int64_t k); /* member k, 0-based, for ?u */
extern Uset *UsetUniv;
void uset_init(void);
bool cvuset(Value v, Uset **out);
Uset *mustuset(Value v);

/* ---- Structures (struct.c) ---- */

List *newlist(int64_t n);
void list_append(List *l, Value v);
void list_pushfront(List *l, Value v);
static inline Value *list_at(List *l, int64_t i) { return &l->e[l->off + i]; }
Record *construct(RecordType *t, Value *a, int n);
int recfield(RecordType *t, Str name);
Table *newtable(Value dflt);
Set *newset(void);
void omap_init(OMap *m);
bool omap_get(OMap *m, Value k, Value *out);
void omap_put(OMap *m, Value k, Value v);
void omap_del(OMap *m, Value k);
void omap_copy(OMap *dst, const OMap *src);
Value tableelem(Table *t, Value k);
Gen *omapgen(OMap *m, Table *t, int mode);
Value varval(int kind);
int anycmp(Value x, Value y);

/* ---- Images (image.c, trace.c) ---- */

void image(Buf *b, Value v);
void outimage(Buf *b, Value v, int noimage);
String bufstring(Buf *b);

/* ---- Scanning and generators (scan.c) ---- */

void scanenter(Value v, Value *slot);
void scanswap(Value *e);
Gen *newrestorer(Value v, int64_t old);
bool tabto(int64_t p, Value *out, int64_t *old);
extern Gen failgen;

/* ---- Files (file.c) ---- */

void files_init(void);
void files_flush_all(void);
void emit(File *f, const char *b, size_t n, bool end);
void flush_output(void);
File *mustfile(Value v);
int file_lend(File *f, int *stat);
File *file_window(String name, void *binding);
File *file_adopt(FILE *fp, int stat, String name);

/* ---- C functions (cfunc.c) ---- */

const char *ext_typename(Value v);
void ext_image(Buf *b, Value v);
Value ext_copy(Value v);
int ext_cmp(Value x, Value y);
bool readline_(File *f, String *out);

/* ---- Tracing (trace.c) ---- */

bool tracing(void);
void tracecall(Frame *f, int pc, Proc *p, Value *args, int nargs);
void traceat(Frame *f, int pc, const char *msg, size_t n);
void traceco(Frame *f, int pc, const char *kind, Value v, Coexp *next);
void traceback(Buf *b);
void calledfrom(Buf *b, Frame *f);
const char *fileat(Proc *p, int pc, size_t *n);
void varname(Buf *b, Value v);
void listing(Buf *b, int64_t n);
void termdump(void);

/* ---- The interpreter (interp.c) ---- */

Value ref(Frame *f, Operand o);
Value get(Frame *f, Operand o);
Value strproc(String name, int nargs);
Coexp *newcoexp(Proc *p, int entry, Value *init);
int64_t cpumillis(void);
void str_init(void);
void misc_init(void);
void file_fns_init(void);
void scan_init(void);
void struct_init(void);
void strfn_init(void);

#endif
