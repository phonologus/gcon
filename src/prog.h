/*
 * A built program, as the compiler makes it and the encoder writes it
 * (the Go reference's vm.Program, Proc, Instr and the constants of
 * value.go, without the runtime's values). The format's definitions are
 * shared with gcon-vm's loader (../vm/src/format.h).
 */
#ifndef GCON_PROG_H
#define GCON_PROG_H

#include "../vm/src/format.h"
#include "util.h"

typedef struct Operand {
	uint8_t kind; /* enum OperandKind */
	int32_t index;
} Operand;

/* A builtin, by reference: its kind and name identify it, and there is
 * one Builtin for each (see builtin_ref), so pointers compare. */
typedef struct Builtin {
	uint8_t kind; /* enum BuiltinKind */
	const char *name;
} Builtin;

Builtin *builtin_ref(int kind, const char *name, size_t n);
Builtin *op_builtin(const char *key);   /* Ops[key]: must exist */
Builtin *function_builtin(const char *name, size_t n); /* Functions[name], or Unsupported(name) */
Builtin *apply_builtin(void);

typedef struct Instr {
	uint8_t op; /* enum Op */
	Operand dst, x;
	int32_t h;
	Operand *args;
	int32_t nargs;
	Builtin *fn;
	int32_t l1, l2;
	int32_t line;
	int32_t file; /* 0: the procedure's file; k: its files[k-1] */
} Instr;

typedef VEC(Instr) InstrVec;
typedef VEC(const char *) StrVec;
typedef VEC(int) IntVec;

typedef struct Proc {
	const char *name, *file;
	int nparams;
	bool variadic;
	int nlocals;
	StrVec localnames;
	IntVec statics;
	StrVec files;
	int ntemps;
	InstrVec code;
} Proc;

typedef struct RecordType {
	const char *name;
	StrVec fields;
} RecordType;

/* A cset: the ASCII characters and non-ASCII bytes in bits, code points
 * (>= 0x80) in ranges, sorted, disjoint, not adjacent. */
typedef struct CsetRange {
	int32_t lo, hi;
} CsetRange;
typedef struct Cset {
	uint64_t bits[4];
	VEC(CsetRange) ranges;
} Cset;

typedef enum ValKind { V_NULL, V_INT, V_BIG, V_REAL, V_STRING, V_CSET, V_BUILTIN, V_PROC, V_RECORD } ValKind;

typedef struct Value {
	ValKind kind;
	int64_t i;          /* V_INT */
	BigNat big;         /* V_BIG: the magnitude */
	bool neg;           /* V_BIG */
	double r;           /* V_REAL */
	const char *s;      /* V_STRING: the bytes */
	size_t n;           /* V_STRING: their number */
	int unit;           /* V_STRING: 0 text, 1 bytes */
	Cset *cset;         /* V_CSET */
	Builtin *builtin;   /* V_BUILTIN */
	Proc *proc;         /* V_PROC */
	RecordType *record; /* V_RECORD */
} Value;

typedef struct Prog {
	VEC(Value) globals;
	StrVec globalnames;
	VEC(Value) consts;
	int64_t main;    /* global index of main, or -1 */
	bool *invocable; /* by global; NULL: all (invocable all) */
} Prog;

/* Csets, as the Go reference's vm/cset.go builds them. */
Cset *cset_new(void);
Cset *cset_union(const Cset *a, const Cset *b);
Cset *cset_complement(const Cset *c);
Cset *cset_from(const char *s, size_t n, int unit);
void cset_addrunes(Cset *c, const CsetRange *rs, size_t n);

/* encode writes p's encoding to out, with the build information keys
 * and values (ninfo of each, written sorted by key). It returns an error
 * message, or NULL. */
const char *encode(const Prog *p, const char *const *infokeys, const char *const *infovals, size_t ninfo,
	Buf *out);

#endif
