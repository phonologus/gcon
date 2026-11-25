/*
 * spike.c: measures a C dispatch loop for gcon's VM against the Go one.
 *
 * The same three loops as gcon's experiments/valuerep (an integer loop,
 * a list subscript read through a variable, a real loop), with the same
 * three-address instructions over frame slots, plus a procedure call
 * loop. Values are 16-byte tagged structs; dispatch is by computed goto
 * (or a switch, with -DSWITCH); frames come from a contiguous stack and
 * are released on return. It is an experiment, not part of gcon-vm; see
 * docs/design.md for the results.
 *
 *	cc -O2 -o spike spike.c && ./spike [iterations]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { kNull, kInt, kReal, kList, kListElem } Kind;

typedef struct Value {
	uint8_t k;
	uint32_t aux; /* an index, for kListElem */
	union {
		int64_t i;
		double r;
		void *p;
	} u;
} Value;

typedef struct {
	Value *elems;
	size_t n;
} List;

enum { opAdd, opLE, opIndex, opDeref, opJump, opCall, opReturn, opMove, opHalt };

typedef struct {
	uint8_t op;
	int32_t a, b, c, l;
} Instr;

/* A procedure: its code and frame size. */
typedef struct {
	Instr *code;
	int nslots;
} Proc;

/* The frame stack: frames are pushed on call and popped on return. */
static Value stack[1 << 16];

static inline Value mkint(int64_t i) { return (Value){.k = kInt, .u.i = i}; }
static inline Value mkreal(double r) { return (Value){.k = kReal, .u.r = r}; }

/*
 * run executes code with frame f. The callee of opCall is the static
 * callee (enough for the measurement); its arguments are slots b and
 * b+1, and its result goes to slot a.
 */
static Proc *callee;

static void run(Instr *code, Value *f) {
	Instr *pc = code;
	Value *sp = f + 64; /* the next frame */
	/* a return stack of (pc, frame, result slot) */
	struct { Instr *pc, *code; Value *f; int32_t dst; } rs[1024];
	int rsp = 0;
#ifndef SWITCH
	static void *labels[] = {&&L_opAdd, &&L_opLE, &&L_opIndex, &&L_opDeref,
		&&L_opJump, &&L_opCall, &&L_opReturn, &&L_opMove, &&L_opHalt};
#define DISPATCH goto *labels[pc->op]
#define CASE(x) L_##x:
#define NEXT DISPATCH
	DISPATCH;
#else
#define CASE(x) case x:
#define NEXT continue
	for (;;) switch (pc->op) {
#endif
	CASE(opAdd) {
		Value x = f[pc->b], y = f[pc->c];
		if (x.k == kInt) {
			int64_t s;
			if (__builtin_add_overflow(x.u.i, y.u.i, &s)) abort();
			f[pc->a] = mkint(s);
		} else
			f[pc->a] = mkreal(x.u.r + y.u.r);
		pc++;
		NEXT;
	}
	CASE(opLE) {
		pc = f[pc->b].u.i <= f[pc->c].u.i ? pc + 1 : code + pc->l;
		NEXT;
	}
	CASE(opIndex) {
		f[pc->a] = (Value){.k = kListElem, .aux = (uint32_t)(f[pc->c].u.i - 1), .u.p = f[pc->b].u.p};
		pc++;
		NEXT;
	}
	CASE(opDeref) {
		Value v = f[pc->b];
		if (v.k == kListElem) f[pc->a] = ((List *)v.u.p)->elems[v.aux];
		pc++;
		NEXT;
	}
	CASE(opJump) {
		pc = code + pc->l;
		NEXT;
	}
	CASE(opCall) {
		/* push a frame for callee: arguments to its slots 0 and 1,
		   its other slots &null */
		Value *nf = sp;
		nf[0] = f[pc->b];
		nf[1] = f[pc->b + 1];
		for (int i = 2; i < callee->nslots; i++) nf[i] = (Value){.k = kNull};
		rs[rsp].pc = pc + 1;
		rs[rsp].code = code;
		rs[rsp].f = f;
		rs[rsp].dst = pc->a;
		rsp++;
		sp += callee->nslots;
		f = nf;
		code = callee->code;
		pc = code;
		NEXT;
	}
	CASE(opReturn) {
		Value v = f[pc->b];
		rsp--;
		sp = f;
		f = rs[rsp].f;
		f[rs[rsp].dst] = v;
		pc = rs[rsp].pc;
		code = rs[rsp].code;
		NEXT;
	}
	CASE(opMove) {
		f[pc->a] = f[pc->b];
		pc++;
		NEXT;
	}
	CASE(opHalt) { return; }
#ifdef SWITCH
	}
#endif
}

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Slots: 0 i, 1 n, 2 x, 3 one, 4 k407, 5 L, 6 three, 7 v, 8 r, 9 k1.5 */
static void frame(Value *f, int64_t n, List *l) {
	f[0] = mkint(1);
	f[1] = mkint(n);
	f[2] = (Value){.k = kNull};
	f[3] = mkint(1);
	f[4] = mkint(407);
	f[5] = (Value){.k = kList, .u.p = l};
	f[6] = mkint(3);
	f[7] = (Value){.k = kNull};
	f[8] = mkreal(0);
	f[9] = mkreal(1.5);
}

int main(int argc, char **argv) {
	int64_t n = argc > 1 ? atoll(argv[1]) : 200000000;
	Value elems[3] = {mkint(10), mkint(20), mkint(30)};
	List l = {elems, 3};

	Instr intLoop[] = {
		{opLE, 0, 0, 1, 4}, {opAdd, 2, 0, 4, 0}, {opAdd, 0, 0, 3, 0},
		{opJump, 0, 0, 0, 0}, {opHalt, 0, 0, 0, 0}};
	Instr indexLoop[] = {
		{opLE, 0, 0, 1, 5}, {opIndex, 7, 5, 6, 0}, {opDeref, 7, 7, 0, 0},
		{opAdd, 0, 0, 3, 0}, {opJump, 0, 0, 0, 0}, {opHalt, 0, 0, 0, 0}};
	Instr realLoop[] = {
		{opLE, 0, 0, 1, 4}, {opAdd, 8, 8, 9, 0}, {opAdd, 0, 0, 3, 0},
		{opJump, 0, 0, 0, 0}, {opHalt, 0, 0, 0, 0}};
	struct {
		const char *name;
		Instr *code;
	} loops[] = {{"int x := i + 407", intLoop}, {"index v := L[3]", indexLoop},
		{"real r := r + 1.5", realLoop}};
	for (int k = 0; k < 3; k++) {
		Value *f = stack;
		frame(f, n, &l);
		double t0 = now();
		run(loops[k].code, f);
		double t = now() - t0;
		printf("%-22s %6.2f ns/iteration\n", loops[k].name, t / n * 1e9);
		if (f[0].u.i != n + 1) abort();
	}

	/* The call loop: while i <= n do { v := q(i, n); i := i + 1 }. */
	Instr all[] = {
		/* 0 */ {opLE, 0, 0, 1, 4},
		/* 1 */ {opCall, 7, 0, 0, 0}, /* v := q(f[0], f[1]) — arguments i, n */
		/* 2 */ {opAdd, 0, 0, 3, 0},
		/* 3 */ {opJump, 0, 0, 0, 0},
		/* 4 */ {opHalt, 0, 0, 0, 0},
	};
	/* q(a, b): local c; c := a; return b */
	Instr qcode[] = {{opMove, 2, 0, 0, 0}, {opReturn, 0, 1, 0, 0}};
	Proc q = {qcode, 3};
	callee = &q;
	{
		Value *f = stack;
		frame(f, n, &l);
		double t0 = now();
		run(all, f);
		double t = now() - t0;
		printf("%-22s %6.2f ns/iteration\n", "call v := q(i, n)", t / n * 1e9);
		if (f[0].u.i != n + 1 || f[7].u.i != n) abort();
	}
	return 0;
}
