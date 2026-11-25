/*
 * The interpreter (gcon's vm.go, coexpr.go, errors.go): a loop over the
 * running frame's instructions that never recurses in C for Icon-level
 * control. Procedure call, return, suspension and resumption switch
 * frames; a suspended procedure is a frame held in a temporary of its
 * caller; a co-expression is a chain of frames, and switching
 * co-expressions switches the running chain. A run-time error unwinds
 * to run() by longjmp, which converts it to failure if &error allows,
 * or reports it.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

VM vm;

/* ---- Errors ---- */

static const struct {
	int num;
	const char *text;
} errors[] = {
	{101, "integer expected or out of range"}, {102, "numeric expected"},
	{103, "string expected"}, {104, "cset expected"}, {105, "file expected"},
	{106, "procedure or integer expected"}, {107, "record expected"},
	{108, "list expected"}, {109, "string or file expected"},
	{110, "string or list expected"}, {111, "variable expected"},
	{112, "invalid type to size operation"}, {113, "invalid type to random operation"},
	{114, "invalid type to subscript operation"}, {115, "structure expected"},
	{116, "invalid type to element generator"}, {117, "missing main procedure"},
	{118, "co-expression expected"}, {119, "set expected"},
	{120, "two csets or two sets expected"}, {121, "function not supported"},
	{122, "set or table expected"}, {123, "invalid type"}, {124, "table expected"},
	{125, "list, record, or set expected"}, {126, "list or record expected"},
	{131, "external expected"}, {132, "incorrect external type"},
	{133, "invalid external value"}, {134, "malformed external value"},
	{201, "division by zero"}, {202, "remaindering by zero"}, {203, "integer overflow"},
	{204, "real overflow, underflow, or division by zero"}, {205, "invalid value"},
	{206, "negative first argument to real exponentiation"}, {207, "invalid field name"},
	{208, "second and third arguments to map of unequal length"},
	{209, "invalid second argument to open"}, {210, "non-ascending arguments to detab/entab"},
	{211, "by value equal to zero"}, {212, "attempt to read file not open for reading"},
	{213, "attempt to write file not open for writing"}, {214, "input/output error"},
	{215, "attempt to refresh &main"}, {216, "external function not found"},
	{300, "interrupted"}, {301, "evaluation stack overflow"}, {302, "memory violation"},
	{303, "inadequate space for evaluation stack"}, {304, "inadequate space in qualifier list"},
	{305, "inadequate space for static allocation"}, {306, "inadequate space in string region"},
	{307, "inadequate space in block region"}, {308, "system stack overflow in co-expression"},
	{402, "program not compiled with debugging option"}, {500, "program malfunction"},
};

const char *errtext(int num) {
	for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
		if (errors[i].num == num)
			return errors[i].text;
	return "";
}

static _Noreturn void raise_(int num, Value v, bool has, bool fatal) {
	vm.rerrnum = num;
	vm.rerrval = v;
	vm.rerrhas = has;
	vm.rerrfatal = fatal;
	longjmp(vm.jmp, 1);
}

void runerr(int num) { raise_(num, NONE, false, false); }
void runerrv(int num, Value v) { raise_(num, v, true, false); }
void runerr_fatal(int num) { raise_(num, NONE, false, true); }

void vm_exit(int code) {
	if (vm.co)
		termdump();
	files_flush_all();
	exit(code);
}

/* convert converts the error being raised to failure if &error allows,
 * reporting whether it did; the running frame then continues at the
 * failing instruction's failure label. */
static bool convert(void) {
	vm.errnum = vm.rerrnum;
	vm.errval = vm.rerrhas ? vm.rerrval : NULLV;
	vm.haveerrval = vm.rerrhas;
	Frame *f = vm.cur;
	if (vm.errorkw == 0 || !f || f->pc >= f->proc->ncode || vm.rerrfatal)
		return false;
	Instr *in = &f->proc->code[f->pc];
	switch (in->op) {
	case OpOper: case OpAssign: case OpActivate: case OpScanEnter:
		break;
	case OpCall: case OpResume:
		f->temps[in->h] = NONE;
		break;
	default:
		return false;
	}
	vm.errorkw--;
	if (vm.haspartial) {
		emit(vm.partialfile, vm.partial.b ? vm.partial.b : "", vm.partial.n, true);
		vm.haspartial = false;
	}
	f->pc = in->l2;
	return true;
}

static _Noreturn void report(void) {
	flush_output();
	Buf b = {0};
	bufprintf(&b, "\nRun-time error %d\n", vm.rerrnum);
	Frame *f = vm.cur;
	if (f && f->pc < f->proc->ncode) {
		size_t n;
		const char *file = fileat(f->proc, f->pc, &n);
		bufputs(&b, "File ");
		bufadd(&b, file, n);
		bufprintf(&b, "; Line %d\n", f->proc->code[f->pc].line);
	}
	bufputs(&b, errtext(vm.rerrnum));
	bufputc(&b, '\n');
	if (vm.rerrhas) {
		bufputs(&b, "offending value: ");
		outimage(&b, vm.rerrval, 0);
		bufputc(&b, '\n');
	}
	traceback(&b);
	vm.errfile->closed = false;
	emit(vm.errfile, b.b, b.n, true);
	if (vm.haspartial && vm.partialfile && vm.partialfile->w)
		emit(vm.partialfile, vm.partial.b ? vm.partial.b : "", vm.partial.n, false);
	vm_exit(1);
}

/* ---- Frames ---- */

/* Frames that return or fail without ever having suspended are recycled
 * at once (docs/design.md, "Frames"): nothing else can refer to them.
 * Free frames are kept by their number of slots, linked through caller;
 * a collection forgets them (the collector then frees them). */
enum { MaxRecycled = 64 };
extern int64_t stress; /* gc.c: GCONVMSTRESS */
static Frame *recycled[MaxRecycled];

void frames_forget(void) { memset(recycled, 0, sizeof recycled); }

/* recycle frees frame f, which has returned or failed, producing v,
 * unless something may still refer to it: it has suspended, or v is a
 * substring of one of its locals. */
static void recycle(Frame *f, Value v) {
	int slots = f->proc->nlocals + f->proc->ntemps;
	if (f->suspended || slots >= MaxRecycled)
		return;
	if (VKIND(v) == KSubstr) {
		Value r = ((Substr *)v.w.p)->v;
		if (VKIND(r) == KRef && (Value *)r.w.p >= f->slots && (Value *)r.w.p < f->slots + slots)
			return;
	}
	if (stress) {
		/* Testing: poison it instead, so that any later use shows. */
		memset(f->slots, 0xA5, (size_t)slots * sizeof(Value));
		f->proc = NULL;
		return;
	}
	f->caller = recycled[slots];
	recycled[slots] = f;
}

static Frame *newframe(Proc *p, Value *args, int nargs) {
	int slots = p->nlocals + p->ntemps;
	Frame *f;
	if (slots < MaxRecycled && recycled[slots]) {
		f = recycled[slots];
		recycled[slots] = f->caller;
	} else
		f = gcalloc(sizeof *f + (size_t)slots * sizeof(Value), T_FRAME, false);
	f->proc = p;
	f->locals = f->slots;
	f->temps = f->slots + p->nlocals;
	f->pc = 0;
	f->suspended = 0;
	f->caller = NULL;
	for (int i = 0; i < p->nlocals; i++)
		f->locals[i] = NULLV;
	memset(f->temps, 0, (size_t)p->ntemps * sizeof(Value));
	int k = p->nparams;
	if (p->variadic) {
		k--;
		List *rest = newlist(0);
		for (int i = k; i < nargs; i++)
			list_append(rest, args[i]);
		f->locals[k] = ptrval(KList, rest);
	}
	for (int i = 0; i < k && i < nargs; i++)
		f->locals[i] = args[i];
	return f;
}

Value get(Frame *f, Operand o) {
	switch (o.kind) {
	case TempOp: return f->temps[o.index];
	case LocalOp: return f->locals[o.index];
	case GlobalOp: return vm.globals[o.index];
	case ConstOp: return vm.prog->consts[o.index];
	}
	abort();
}

Value ref(Frame *f, Operand o) {
	switch (o.kind) {
	case LocalOp: return ptrval(KRef, &f->locals[o.index]);
	case GlobalOp: return ptrval(KRef, &vm.globals[o.index]);
	}
	return get(f, o);
}

static bool assignop(Frame *f, Operand o, Value v) {
	switch (o.kind) {
	case LocalOp:
		f->locals[o.index] = v;
		return true;
	case GlobalOp:
		vm.globals[o.index] = v;
		return true;
	case TempOp: {
		bool isv;
		bool ok = assignvar(f->temps[o.index], v, &isv);
		if (isv)
			return ok;
	}
	}
	runerrv(111, deref(get(f, o)));
}

static bool isvarop(Frame *f, Operand o) {
	switch (o.kind) {
	case LocalOp: case GlobalOp: return true;
	case TempOp: return isvar(f->temps[o.index]);
	}
	return false;
}

enum { ScratchRoom = 6 };

/* scratchargs returns the dereferenced values of ops in the VM's scratch
 * buffer, with room for adjustment's padding. */
static Value *scratchargs(Frame *f, Operand *ops, int n, int need) {
	int cap = (n > need ? n : need) + ScratchRoom;
	if (vm.nscratch < cap) {
		vm.nscratch = cap > 16 ? cap * 2 : 32;
		vm.scratch = xcalloc((size_t)vm.nscratch, sizeof(Value)); /* a root: zeroed */
	}
	for (int i = 0; i < n; i++)
		vm.scratch[i] = deref(get(f, ops[i]));
	return vm.scratch;
}

/* adjust fits the arguments of a call of builtin b to its parameters:
 * missing ones are &null, extra ones dropped. The buffer has room. */
static int adjust(Builtin *b, Value *args, int n) {
	switch (b->adjmode) {
	case AdjMin:
		for (; n < b->adjn; n++)
			args[n] = NULLV;
		return n;
	case AdjExact:
		for (; n < b->adjn; n++)
			args[n] = NULLV;
		return b->adjn;
	}
	return n;
}

/* retderef: a local of the returning frame is dereferenced; other
 * variables are produced as variables. */
static Value retderef(Frame *f, Value v) {
	if (VKIND(v) == KRef) {
		Value *p = v.w.p;
		if (p >= f->locals && p < f->locals + f->proc->nlocals)
			return *p;
	}
	return v;
}

static int64_t framecost(Proc *p) { return 9 + p->nlocals; }

static void push(Proc *p) {
	int64_t c = framecost(p);
	if (vm.co->stack + c > vm.stacklimit)
		runerr_fatal(301);
	vm.co->stack += c;
}

static void pop(Frame *f) {
	if (f->caller)
		vm.co->stack -= framecost(f->proc);
}

/* ---- Co-expressions ---- */

Coexp *newcoexp(Proc *p, int entry, Value *init) {
	Coexp *c = gcnew(sizeof *c, T_COEXP);
	c->serial = ++vm.serial_coexps;
	c->proc = p;
	c->entry = entry;
	c->init = init;
	return c;
}

static Frame *transfer(Coexp *c, Value v, bool ok) {
	vm.co = c;
	Frame *f;
	switch (c->state) {
	case CoNew: {
		Proc *p = c->proc;
		f = gcnew(sizeof *f + (size_t)(p->nlocals + p->ntemps) * sizeof(Value), T_FRAME);
		f->proc = p;
		f->locals = f->slots;
		f->temps = f->slots + p->nlocals;
		memcpy(f->locals, c->init, (size_t)p->nlocals * sizeof(Value));
		memset(f->temps, 0, (size_t)p->ntemps * sizeof(Value));
		f->pc = c->entry;
		f->caller = NULL;
		break;
	}
	case CoResult:
		f = c->frame;
		break;
	case CoWaiting: {
		f = c->frame;
		Instr *in = &f->proc->code[f->pc];
		if (ok) {
			f->temps[in->dst.index] = v;
			f->pc = in->l1;
		} else
			f->pc = in->l2;
		break;
	}
	default:
		abort();
	}
	c->state = CoRunning;
	c->frame = NULL;
	vm.cur = f;
	return f;
}

static Frame *activate(Frame *f, Instr *in, Value target, Value v) {
	if (VKIND(target) != KCoexp)
		runerrv(118, target);
	Coexp *c = target.w.p;
	if (c->state == CoDone) {
		f->pc = in->l2;
		return f;
	}
	if (tracing())
		traceco(f, f->pc, "activate", v, c);
	Coexp *cur = vm.co;
	cur->frame = f;
	cur->state = CoWaiting;
	c->activator = cur;
	return transfer(c, v, true);
}

static Frame *coret(Frame *f, Value v, int resume) {
	Coexp *c = vm.co;
	if (tracing())
		traceco(f, f->pc, "return", v, c->activator);
	c->results++;
	f->pc = resume;
	c->frame = f;
	c->state = CoResult;
	return transfer(c->activator, v, true);
}

static Frame *cofail(void) {
	Coexp *c = vm.co;
	if (tracing())
		traceco(vm.cur, vm.cur->pc, "fail", NONE, c->activator);
	c->state = CoDone;
	return transfer(c->activator, NONE, false);
}

/* ---- Generators in temporaries ---- */

static bool gennext(Value h, Value *out) {
	Gen *g = h.w.p;
	if (!g) { /* a restoring builtin's handle: restore &pos and fail */
		vm.pos = (int64_t)VN(h);
		return false;
	}
	return g->next(g, out);
}

static void next(Frame *f, Instr *in, Value h) {
	Value v;
	if (gennext(h, &v)) {
		f->temps[in->h] = h;
		f->temps[in->dst.index] = v;
		f->pc = in->l1;
	} else {
		f->temps[in->h] = NONE;
		f->pc = in->l2;
	}
}

/* ---- Fast paths ---- */

static bool operint(Frame *f, Instr *in, Builtin *b) {
	int op = b->intop;
	Value v = NONE;
	bool ok;
	if (op >= OpSize) {
		Value x = deref(get(f, in->args[0]));
		Value y = in->nargs > 1 ? deref(get(f, in->args[1])) : NONE;
		switch (op) {
		case OpSize:
			switch (VKIND(x)) {
			case KString: v = intval((int64_t)VN(x)); break;
			case KList: v = intval(((List *)x.w.p)->n); break;
			case KTable: v = intval(((Table *)x.w.p)->m.live); break;
			case KSet: v = intval(((Set *)x.w.p)->m.live); break;
			case KRecord: v = intval(((Record *)x.w.p)->t->nfields); break;
			case KCset: v = intval(cset_size(x.w.p)); break;
			default: return false;
			}
			ok = true;
			break;
		case OpPlus:
			if (VKIND(x) != KInt && VKIND(x) != KReal)
				return false;
			v = x, ok = true;
			break;
		case OpNeg:
			if (VKIND(x) == KInt && x.w.i != INT64_MIN)
				v = intval(-x.w.i);
			else if (VKIND(x) == KReal)
				v = realval(-x.w.r);
			else
				return false;
			ok = true;
			break;
		case OpIdent:
			v = y, ok = identical(x, y);
			break;
		default:
			v = y, ok = !identical(x, y);
		}
	} else {
		Value xv = deref(get(f, in->args[0])), yv = deref(get(f, in->args[1]));
		if (VKIND(xv) != KInt || VKIND(yv) != KInt)
			return false;
		int64_t x = xv.w.i, y = yv.w.i, r;
		switch (op) {
		case IntAdd:
			if (!addint(x, y, &r))
				return false;
			v = intval(r), ok = true;
			break;
		case IntSub:
			if (!subint(x, y, &r))
				return false;
			v = intval(r), ok = true;
			break;
		case IntMul:
			if (!mulint(x, y, &r))
				return false;
			v = intval(r), ok = true;
			break;
		case IntLT: v = yv, ok = x < y; break;
		case IntLE: v = yv, ok = x <= y; break;
		case IntGT: v = yv, ok = x > y; break;
		case IntGE: v = yv, ok = x >= y; break;
		case IntEQ: v = yv, ok = x == y; break;
		default: v = yv, ok = x != y;
		}
	}
	if (ok) {
		f->temps[in->dst.index] = v;
		f->pc = in->l1;
	} else
		f->pc = in->l2;
	return true;
}

/* callable converts v, not a procedure, builtin or record constructor,
 * to something a call can invoke with nargs arguments. */
static Value callable(Value v, int nargs) {
	int64_t i;
	if (cvint(v, &i))
		return intval(i);
	String s;
	if (cvstr(v, &s)) {
		Value p = strproc(s, nargs);
		if (VKIND(p) != KNone)
			return p;
	}
	runerrv(106, v);
}

static Value *applyargs(Value x, int *n) {
	if (VKIND(x) == KList) {
		List *l = x.w.p;
		Value *a = gcnew((size_t)(l->n + ScratchRoom + 8) * sizeof(Value), T_VALUES);
		memcpy(a, list_at(l, 0), (size_t)l->n * sizeof(Value));
		*n = (int)l->n;
		return a;
	}
	if (VKIND(x) == KRecord) {
		Record *r = x.w.p;
		Value *a = gcnew((size_t)(r->t->nfields + ScratchRoom + 8) * sizeof(Value), T_VALUES);
		memcpy(a, r->f, (size_t)r->t->nfields * sizeof(Value));
		*n = r->t->nfields;
		return a;
	}
	runerrv(126, x);
}

static char *fromstr(Frame *f) {
	Buf b = {0};
	calledfrom(&b, f);
	return cstr(bufstring(&b, TextUnit));
}

/* ---- The loop ---- */

/* Dispatch: computed goto where the compiler has it (GCC, Clang), each
 * handler jumping straight to the next; else a switch. NEXT is the safe
 * point: a collection runs there, when one is due. */
#if defined(__GNUC__) && !defined(GCONVM_SWITCH)
#define CASE(op) L_##op:
#define DEFAULT L_bad: __attribute__((unused));
#define NEXT                                         \
	do {                                         \
		if (gc_pending) {                    \
			vm.cur = f;                  \
			gc_collect();                \
		}                                    \
		in = &f->proc->code[f->pc];          \
		goto *labels[in->op];                \
	} while (0)
#else
#define CASE(op) case op:
#define DEFAULT default:
#define NEXT continue
#endif

static void loop(Frame *f) {
	vm.cur = f;
	Instr *in;
#if defined(__GNUC__) && !defined(GCONVM_SWITCH)
	static void *const labels[NumOps] = {
		&&L_OpGoto, &&L_OpIGoto, &&L_OpSetLabel, &&L_OpMove, &&L_OpMoveVar,
		&&L_OpAssign, &&L_OpOper, &&L_OpCall, &&L_OpResume, &&L_OpReturn,
		&&L_OpFail, &&L_OpSuspend, &&L_OpOnce, &&L_OpIsNull, &&L_OpCreate,
		&&L_OpActivate, &&L_OpCoRet, &&L_OpCoFail, &&L_OpScanEnter, &&L_OpScanSwap,
	};
	NEXT;
	{
		{
#else
	for (;;) {
		if (gc_pending) {
			vm.cur = f;
			gc_collect();
		}
		in = &f->proc->code[f->pc];
		switch (in->op) {
#endif
		CASE(OpGoto)
			f->pc = in->l1;
			NEXT;
		CASE(OpIGoto)
			f->pc = (int)f->temps[in->x.index].w.i;
			NEXT;
		CASE(OpSetLabel)
			f->temps[in->dst.index] = intval(in->l1);
			f->pc++;
			NEXT;
		CASE(OpMove)
			f->temps[in->dst.index] = deref(get(f, in->x));
			f->pc++;
			NEXT;
		CASE(OpMoveVar)
			f->temps[in->dst.index] = ref(f, in->x);
			f->pc++;
			NEXT;
		CASE(OpAssign) {
			Builtin *ab = in->fnp;
			if (ab && (!strcmp(ab->name, ":=:") || !strcmp(ab->name, "<->"))) {
				if (!isvarop(f, in->dst))
					runerrv(111, deref(get(f, in->dst)));
				if (!isvarop(f, in->x))
					runerrv(111, deref(get(f, in->x)));
			}
			if (in->nargs == 1) {
				Value dv = ref(f, in->dst), ov = ref(f, in->args[0]);
				if (VKIND(dv) == KSubstr && VKIND(ov) == KSubstr &&
				    ((Substr *)dv.w.p)->v.h == ((Substr *)ov.w.p)->v.h &&
				    ((Substr *)dv.w.p)->v.w.p == ((Substr *)ov.w.p)->v.w.p) {
					Substr *d = dv.w.p, *o = ov.w.p;
					int64_t k = d->n;
					if (!assignop(f, in->dst, deref(get(f, in->x)))) {
						f->pc = in->l2;
						NEXT;
					}
					if (o->pos > d->pos)
						o->pos += d->n - k;
					f->pc = in->l1;
					NEXT;
				}
			}
			if (assignop(f, in->dst, deref(get(f, in->x))))
				f->pc = in->l1;
			else
				f->pc = in->l2;
			NEXT;
		}
		CASE(OpScanEnter)
			f->temps[in->dst.index] = scanenter(deref(get(f, in->x)));
			f->pc++;
			NEXT;
		CASE(OpScanSwap)
			scanswap(&f->temps[in->x.index]);
			f->pc++;
			NEXT;
		CASE(OpIsNull)
			f->pc = VKIND(deref(get(f, in->x))) == KNull ? in->l1 : in->l2;
			NEXT;
		CASE(OpOnce)
			if (VKIND(vm.globals[in->x.index]) == KNull) {
				vm.globals[in->x.index] = intval(1);
				f->pc++;
			} else
				f->pc = in->l1;
			NEXT;
		CASE(OpOper) {
			Builtin *b = in->fnp;
			if (b->intop != IntNone && operint(f, in, b))
				NEXT;
			Value *args = scratchargs(f, in->args, in->nargs, 0);
			if (b->reffirst)
				args[0] = ref(f, in->args[0]);
			Value v;
			if (b->fn(b, args, in->nargs, &v)) {
				f->temps[in->dst.index] = v;
				f->pc = in->l1;
			} else
				f->pc = in->l2;
			NEXT;
		}
		CASE(OpCall) {
			Value fv = deref(get(f, in->x));
			int n = in->nargs;
			/* The arguments are in the scratch buffer, which has room
			 * for adjust's padding (no builtin takes more than six),
			 * unless they come from p ! x. */
			Value *args = scratchargs(f, in->args, n, 0);
			bool apply = in->fnp == &applyop;
			if (apply)
				args = applyargs(args[0], &n);
			switch (VKIND(fv)) {
			case KProc: case KBuiltin: case KRecordType: case KInt:
				break;
			default:
				fv = callable(fv, n);
				if (VKIND(fv) == KBuiltin && ((Builtin *)fv.w.p)->noderef)
					for (int i = 0; i < in->nargs; i++)
						args[i] = ref(f, in->args[i]);
			}
			switch (VKIND(fv)) {
			case KProc: {
				Proc *p = fv.w.p;
				push(p);
				Frame *nf = newframe(p, args, n);
				if (tracing())
					tracecall(f, f->pc, p, nf->locals, p->nparams);
				vm.level++;
				nf->caller = f;
				f = nf;
				vm.cur = f;
				break;
			}
			case KBuiltin: {
				Builtin *b = fv.w.p;
				if (b->reffirst && n > 0 && !apply)
					args[0] = ref(f, in->args[0]);
				if (in->x.kind != ConstOp)
					n = adjust(b, args, n);
				if (b->restore) {
					Value v;
					int64_t old;
					if (b->restore(b, args, n, &v, &old)) {
						f->temps[in->dst.index] = v;
						f->temps[in->h] = idxval(KGen, NULL, old);
						f->pc = in->l1;
					} else {
						f->temps[in->h] = NONE;
						f->pc = in->l2;
					}
				} else if (b->gen) {
					Gen *g = b->gen(b, args, n);
					next(f, in, ptrval(KGen, g));
				} else {
					f->temps[in->h] = NONE;
					Value v;
					if (b->fn(b, args, n, &v)) {
						f->temps[in->dst.index] = v;
						f->pc = in->l1;
					} else
						f->pc = in->l2;
				}
				NEXT;
			}
			case KRecordType:
				f->temps[in->h] = NONE;
				f->temps[in->dst.index] = ptrval(KRecord, construct(fv.w.p, args, n));
				f->pc = in->l1;
				NEXT;
			case KInt: {
				f->temps[in->h] = NONE;
				int64_t i = fv.w.i;
				if (i >= 1 && i <= n) {
					f->temps[in->dst.index] = args[i - 1];
					f->pc = in->l1;
				} else if (i < 0 && -i <= n) {
					f->temps[in->dst.index] = args[n + i];
					f->pc = in->l1;
				} else
					f->pc = in->l2;
				NEXT;
			}
			default:
				runerrv(106, fv);
			}
			NEXT;
		}
		CASE(OpResume) {
			Value hv = f->temps[in->h];
			switch (VKIND(hv)) {
			case KNone:
				f->pc = in->l2;
				break;
			case KFrame: {
				Frame *h = hv.w.p;
				push(h->proc);
				if (tracing()) {
					Buf b = {0};
					bufstr(&b, h->proc->name);
					bufputs(&b, " resumed");
					traceat(f, f->pc, b.b, b.n);
				free(b.b);
				}
				vm.level++;
				h->caller = f;
				f = h;
				vm.cur = f;
				break;
			}
			case KGen:
				next(f, in, hv);
				NEXT;
			default:
				abort();
			}
			NEXT;
		}
		CASE(OpReturn) {
			Value v = retderef(f, ref(f, in->x));
			vm.level--;
			pop(f);
			if (tracing()) {
				Buf b = {0};
				bufstr(&b, f->proc->name);
				bufputs(&b, " returned ");
				outimage(&b, v, 0);
				traceat(f, f->pc, b.b, b.n);
				free(b.b);
			}
			Frame *c = f->caller;
			if (!c) {
				if (vm.co != vm.main) {
					f = cofail();
					NEXT;
				}
				return;
			}
			Instr *ci = &c->proc->code[c->pc];
			c->temps[ci->dst.index] = v;
			c->temps[ci->h] = NONE;
			c->pc = ci->l1;
			recycle(f, v);
			f = c;
			vm.cur = f;
			NEXT;
		}
		CASE(OpFail) {
			vm.level--;
			pop(f);
			if (tracing()) {
				Buf b = {0};
				bufstr(&b, f->proc->name);
				bufputs(&b, " failed");
				traceat(f, f->pc, b.b, b.n);
				free(b.b);
			}
			Frame *c = f->caller;
			if (!c) {
				if (vm.co != vm.main) {
					f = cofail();
					NEXT;
				}
				return;
			}
			Instr *ci = &c->proc->code[c->pc];
			c->temps[ci->h] = NONE;
			c->pc = ci->l2;
			recycle(f, NONE);
			f = c;
			vm.cur = f;
			NEXT;
		}
		CASE(OpSuspend) {
			Value v = retderef(f, ref(f, in->x));
			vm.level--;
			pop(f);
			if (tracing()) {
				Buf b = {0};
				bufstr(&b, f->proc->name);
				bufputs(&b, " suspended ");
				outimage(&b, v, 0);
				traceat(f, f->pc, b.b, b.n);
				free(b.b);
			}
			f->pc = in->l1;
			f->suspended = 1;
			Frame *c = f->caller;
			if (!c) {
				if (vm.co != vm.main) {
					f = cofail();
					NEXT;
				}
				return;
			}
			Instr *ci = &c->proc->code[c->pc];
			c->temps[ci->dst.index] = v;
			c->temps[ci->h] = ptrval(KFrame, f);
			c->pc = ci->l1;
			f->caller = NULL;
			f = c;
			vm.cur = f;
			NEXT;
		}
		CASE(OpCreate) {
			Value *init = gcnew((size_t)(f->proc->nlocals + 1) * sizeof(Value), T_VALUES);
			memcpy(init, f->locals, (size_t)f->proc->nlocals * sizeof(Value));
			Coexp *c = newcoexp(f->proc, in->l1, init);
			c->from = fromstr(f);
			f->temps[in->dst.index] = ptrval(KCoexp, c);
			f->pc++;
			NEXT;
		}
		CASE(OpActivate) {
			Value v = in->nargs > 0 ? deref(get(f, in->args[0])) : NULLV;
			f = activate(f, in, deref(get(f, in->x)), v);
			NEXT;
		}
		CASE(OpCoRet)
			f = coret(f, deref(get(f, in->x)), in->l1);
			NEXT;
		CASE(OpCoFail)
			f = cofail();
			NEXT;
		DEFAULT
			abort();
		}
	}
}

static void run(Frame *f) {
	for (;;) {
		if (setjmp(vm.jmp) == 0) {
			loop(f);
			return;
		}
		if (!convert())
			report();
		f = vm.cur;
	}
}

/* ---- Linking and starting ---- */

static Builtin *resolve(BuiltinRef *r) {
	char *name = cstr(mkstr(r->name.s, (int64_t)r->name.n, TextUnit));
	Builtin *b = NULL;
	switch (r->kind) {
	case BuiltinOp:
		b = findop(name);
		break;
	case BuiltinFunction:
		b = findfn(r->name.s, r->name.n);
		break;
	case BuiltinAsgn:
		return asgnop(r->name);
	case BuiltinApply:
		return &applyop;
	case BuiltinField:
		return fieldop(r->name);
	case BuiltinUnsupported:
		return unsupported(r->name);
	}
	if (!b) {
		fprintf(stderr, "gcon-vm: %s %s is not implemented yet\n",
			r->kind == BuiltinOp ? "operator" : "function", name);
		exit(3);
	}
	return b;
}

static void linkvalue(Value *v) {
	if (VKIND(*v) == KBuiltin)
		*v = ptrval(KBuiltin, vm.builtins[VN(*v)]);
	else if (VKIND(*v) == KString) /* the loader leaves the ASCII flag unset */
		*v = strval(mkstr(v->w.s, (int64_t)VN(*v), VFLAGS(*v) & SBytes));
}

void link_program(Program *p) {
	vm.prog = p;
	vm.builtins = xmalloc((size_t)(p->nbuiltins + 1) * sizeof(Builtin *));
	for (int i = 0; i < p->nbuiltins; i++)
		vm.builtins[i] = resolve(&p->builtins[i]);
	for (int i = 0; i < p->nprocs; i++) {
		Proc *pr = &p->procs[i];
		for (int k = 0; k < pr->ncode; k++)
			pr->code[k].fnp = pr->code[k].fn >= 0 ? vm.builtins[pr->code[k].fn] : NULL;
	}
	for (int i = 0; i < p->nconsts; i++)
		linkvalue(&p->consts[i]);
	for (int i = 0; i < p->nglobals; i++)
		linkvalue(&p->globals[i]);
}

int run_program(Program *p, const char *progname, int argc, char **argv) {
	vm.subject = textc("");
	vm.pos = 1;
	vm.progname = textc(progname);
	vm.eventcode = vm.eventsource = vm.eventvalue = NULLV;
	vm.errval = NULLV;
	const char *mstk = getenv("MSTKSIZE");
	vm.stacklimit = 500000;
	if (mstk && atoll(mstk) > 1)
		vm.stacklimit = atoll(mstk) / 2;
	files_init();
	vm.globals = xmalloc((size_t)(p->nglobals + 1) * sizeof(Value));
	memcpy(vm.globals, p->globals, (size_t)p->nglobals * sizeof(Value));
	if (setjmp(vm.jmp) != 0)
		report();
	Proc *main = NULL;
	if (p->main >= 0 && VKIND(vm.globals[p->main]) == KProc)
		main = vm.globals[p->main].w.p;
	if (!main)
		runerr(117);
	Value margs[1];
	int nm = 0;
	if (main->nparams > 0) {
		List *l = newlist(0);
		for (int i = 0; i < argc; i++)
			list_append(l, strval(textc(argv[i])));
		margs[0] = ptrval(KList, l);
		nm = 1;
	}
	vm.serial_coexps = 1;
	vm.main = gcnew(sizeof *vm.main, T_COEXP);
	vm.main->serial = 1;
	vm.main->proc = main;
	vm.main->state = CoRunning;
	vm.main->results = 1;
	vm.main->activator = vm.main;
	vm.main->from = "";
	vm.co = vm.main;
	const char *t = getenv("TRACE");
	if (t && *t) {
		char *end;
		long long v = strtoll(t, &end, 10);
		if (*end == 0)
			vm.trace = v;
	}
	Frame *mf = newframe(main, margs, nm);
	if (tracing())
		tracecall(NULL, 0, main, mf->locals, main->nparams);
	vm.level = 1;
	run(mf);
	vm_exit(0);
}
