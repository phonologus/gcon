/*
 * image(), the images in trace messages and error reports (outimage),
 * tracing, the traceback, names of variables, and display's listing
 * (gcon's ops.go Image, trace.go, names.go, substr.go).
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

void image(Buf *b, Value v) {
	v = deref(v);
	switch (VKIND(v)) {
	case KNull:
		bufputs(b, "&null");
		return;
	case KInt:
		itoa_(b, v.w.i);
		return;
	case KBig:
		bigimage(b, v.w.p);
		return;
	case KString: {
		String s = vstr(v);
		quote(b, s.s, (size_t)s.n, s.unit == BytesUnit, '"');
		return;
	}
	case KList: {
		List *l = v.w.p;
		bufprintf(b, "list_%lld(%lld)", (long long)l->serial, (long long)l->n);
		return;
	}
	case KProc:
		bufputs(b, "procedure ");
		bufstr(b, ((Proc *)v.w.p)->name);
		return;
	case KBuiltin:
		bufputs(b, "function ");
		bufputs(b, ((Builtin *)v.w.p)->name);
		return;
	case KExternal:
		ext_image(b, v);
		return;
	case KCoexp: {
		Coexp *c = v.w.p;
		bufprintf(b, "co-expression_%lld(%lld)", (long long)c->serial, (long long)c->results);
		return;
	}
	case KCset:
		imagecset(b, v.w.p);
		return;
	case KReal:
		rtos(b, v.w.r);
		return;
	case KRecord: {
		Record *r = v.w.p;
		bufputs(b, "record ");
		bufstr(b, r->t->name);
		bufprintf(b, "_%lld(%d)", (long long)r->serial, r->t->nfields);
		return;
	}
	case KRecordType:
		bufputs(b, "record constructor ");
		bufstr(b, ((RecordType *)v.w.p)->name);
		return;
	case KFile: {
		File *f = v.w.p;
		if (f->keyword)
			bufadd(b, f->name.s, (size_t)f->name.n);
		else {
			bufputs(b, "file(");
			bufadd(b, f->name.s, (size_t)f->name.n);
			bufputc(b, ')');
		}
		return;
	}
	case KTable: {
		Table *t = v.w.p;
		bufprintf(b, "table_%lld(%lld)", (long long)t->serial, (long long)t->m.live);
		return;
	}
	case KSet: {
		Set *s = v.w.p;
		bufprintf(b, "set_%lld(%lld)", (long long)s->serial, (long long)s->m.live);
		return;
	}
	}
	bufputc(b, '?');
}

enum { StringLimit = 16, ListLimit = 6 };

static void substrimage(Buf *b, Substr *s, int noimage) {
	if (VKIND(s->v) == KVar && ((Var *)s->v.w.p)->kind == VSubject)
		bufputs(b, "&subject");
	else
		outimage(b, deref(s->v), noimage);
	if (s->n == 1)
		bufprintf(b, "[%lld]", (long long)s->pos);
	else
		bufprintf(b, "[%lld+:%lld]", (long long)s->pos, (long long)s->n);
	Value d = deref(s->v);
	if (VKIND(d) == KString && s->pos - 1 + s->n <= (int64_t)VN(d)) {
		bufputs(b, " = ");
		outimage(b, strval(slice(vstr(d), s->pos, s->pos + s->n)), noimage);
	}
}

void outimage(Buf *b, Value v, int noimage) {
	if (VKIND(v) == KVar) {
		Var *x = v.w.p;
		const char *kw = NULL;
		switch (x->kind) {
		case VPos: kw = "&pos = "; break;
		case VSubject: kw = "&subject = "; break;
		case VRandom: kw = "&random = "; break;
		case VTrace: kw = "&trace = "; break;
		case VError: kw = "&error = "; break;
		}
		if (kw) {
			bufputs(b, kw);
			outimage(b, deref(v), noimage);
			return;
		}
	}
	if (VKIND(v) == KSubstr) {
		substrimage(b, v.w.p, noimage);
		return;
	}
	if (isvar(v)) {
		bufputs(b, "(variable = ");
		outimage(b, deref(v), noimage);
		bufputc(b, ')');
		return;
	}
	switch (VKIND(v)) {
	case KString: {
		String s = vstr(v);
		if (runes(s) <= StringLimit) {
			quote(b, s.s, (size_t)s.n, s.unit == BytesUnit, '"');
			return;
		}
		int64_t i = 1;
		for (int k = 0; k < StringLimit; k++)
			i += width(s, i - 1);
		Buf q = {0};
		quote(&q, s.s, (size_t)(i - 1), s.unit == BytesUnit, '"');
		bufadd(b, q.b, q.n - 1);
		bufputs(b, "...\"");
		free(q.b);
		return;
	}
	case KCset: {
		Cset *c = v.w.p;
		if (csetname_(c) || cset_size(c) <= StringLimit) {
			imagecset(b, c);
			return;
		}
		/* The first StringLimit members, as text. */
		String all = cset_string(c);
		Buf m = {0};
		int64_t k = 0;
		if (c->nranges == 0) {
			bufadd(&m, all.s, StringLimit);
		} else {
			for (int64_t i = 0; i < all.n && k < StringLimit; k++) {
				/* members: ASCII and lone bytes are one byte; code points their encoding */
				uint8_t ch = (uint8_t)all.s[i];
				int w = 1;
				if (ch >= 0x80) {
					int32_t r = utf8decode((const uint8_t *)all.s + i, (size_t)(all.n - i), &w);
					if (r == RuneError && w == 1)
						w = 1;
					else if (!cset_hasrune(c, r))
						w = 1;
				}
				bufadd(&m, all.s + i, (size_t)w);
				i += w;
			}
		}
		Buf q = {0};
		quote(&q, m.b, m.n, false, '\'');
		bufadd(b, q.b, q.n - 1);
		bufputs(b, "...'");
		return;
	}
	case KList: {
		List *l = v.w.p;
		int64_t k = l->n;
		if (noimage > 0 && k > 0) {
			bufprintf(b, "list_%lld(%lld)", (long long)l->serial, (long long)k);
			return;
		}
		bufprintf(b, "list_%lld = [", (long long)l->serial);
		for (int64_t i = 0; i < k; i++) {
			if (i >= ListLimit / 2 && i < k - ListLimit / 2) {
				if (i == ListLimit / 2)
					bufputs(b, "...,");
				continue;
			}
			outimage(b, *list_at(l, i), noimage + 1);
			if (i < k - 1)
				bufputc(b, ',');
		}
		bufputc(b, ']');
		return;
	}
	case KRecord: {
		Record *r = v.w.p;
		bufputs(b, "record ");
		bufstr(b, r->t->name);
		bufprintf(b, "_%lld", (long long)r->serial);
		if (r->t->nfields == 0) {
			bufputs(b, "()");
			return;
		}
		if (noimage > 0) {
			bufprintf(b, "(%d)", r->t->nfields);
			return;
		}
		bufputc(b, '(');
		for (int i = 0; i < r->t->nfields; i++) {
			if (i > 0)
				bufputc(b, ',');
			outimage(b, r->f[i], noimage + 1);
		}
		bufputc(b, ')');
		return;
	}
	}
	image(b, v);
}

/* ---- Tracing ---- */

bool tracing(void) {
	if (vm.trace == 0)
		return false;
	vm.trace--;
	return true;
}

const char *fileat(Proc *p, int pc, size_t *n) {
	if (pc >= 0 && pc < p->ncode) {
		int k = p->code[pc].file;
		if (k > 0) {
			*n = p->files[k - 1].n;
			return p->files[k - 1].s;
		}
	}
	*n = p->file.n;
	return p->file.s;
}

static void traceline(const char *file, size_t fn, int line, const char *msg, size_t mn) {
	Buf b = {0};
	if (line > 0) {
		if (fn > 13) {
			file += fn - 13;
			fn = 13;
		}
		bufadd(&b, file, fn);
		for (size_t i = fn; i < 13; i++)
			bufputc(&b, ' ');
		bufprintf(&b, ": %4d  ", line);
	} else
		bufputs(&b, "             :       ");
	for (int64_t i = 0; i < vm.level; i++)
		bufputs(&b, "| ");
	bufadd(&b, msg, mn);
	bufputc(&b, '\n');
	flush_output();
	emit(vm.errfile, b.b, b.n, true);
	free(b.b);
}

void traceat(Frame *f, int pc, const char *msg, size_t n) {
	int line = 0;
	size_t fn = 0;
	const char *file = "";
	if (f && pc >= 0 && pc < f->proc->ncode)
		line = f->proc->code[pc].line;
	if (f)
		file = fileat(f->proc, pc, &fn);
	traceline(file, fn, line, msg, n);
}

void tracecall(Frame *f, int pc, Proc *p, Value *args, int nargs) {
	Buf b = {0};
	bufstr(&b, p->name);
	bufputc(&b, '(');
	for (int i = 0; i < nargs; i++) {
		if (i > 0)
			bufputc(&b, ',');
		outimage(&b, args[i], 0);
	}
	bufputc(&b, ')');
	traceat(f, pc, b.b, b.n);
}

void traceco(Frame *f, int pc, const char *kind, Value v, Coexp *next) {
	Buf b = {0};
	bufstr(&b, f->proc->name);
	bufprintf(&b, "; co-expression_%lld ", (long long)vm.co->serial);
	if (!strcmp(kind, "activate")) {
		bufputs(&b, ": ");
		outimage(&b, v, 0);
		bufputs(&b, " @ ");
	} else if (!strcmp(kind, "return")) {
		bufputs(&b, "returned ");
		outimage(&b, v, 0);
		bufputs(&b, " to ");
	} else
		bufputs(&b, "failed to ");
	bufprintf(&b, "co-expression_%lld", (long long)next->serial);
	traceat(f, pc, b.b, b.n);
}

void calledfrom(Buf *b, Frame *f) {
	Frame *c = f->caller;
	if (!c) {
		if (vm.co->from)
			bufputs(b, vm.co->from);
		return;
	}
	size_t n;
	const char *file = fileat(c->proc, c->pc, &n);
	bufprintf(b, " from line %d in ", c->proc->code[c->pc].line);
	bufadd(b, file, n);
}

/* ---- The traceback ---- */

static void val(Buf *b, Frame *f, Operand o) { outimage(b, deref(get(f, o)), 0); }
static void vari(Buf *b, Frame *f, Operand o) { outimage(b, ref(f, o), 0); }

static void args(Buf *b, Frame *f, Operand *ops, int n) {
	for (int i = 0; i < n; i++) {
		if (i > 0)
			bufputc(b, ',');
		val(b, f, ops[i]);
	}
}

/* converted shows the left operand of binary operator op converted to
 * the type op wants, as iconx shows it. */
static void converted(Buf *b, const char *op, Value x) {
	Value c = x;
	bool ok = false;
	if (strchr("+-*/%^", op[0]) && strlen(op) == 1)
		ok = cvnum(x, &c);
	else if (!strcmp(op, "<") || !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=") ||
		 !strcmp(op, "=") || !strcmp(op, "~="))
		ok = cvnum(x, &c);
	else if (!strcmp(op, "||") || !strcmp(op, "==") || !strcmp(op, "~==") || !strcmp(op, "<<") ||
		 !strcmp(op, "<<=") || !strcmp(op, ">>") || !strcmp(op, ">>=")) {
		String s;
		ok = cvstr(x, &s);
		if (ok)
			c = strval(s);
	} else if (!strcmp(op, "++") || !strcmp(op, "--") || !strcmp(op, "**")) {
		if (VKIND(x) != KSet) {
			Cset *cs;
			ok = cvcset(x, &cs);
			if (ok)
				c = ptrval(KCset, cs);
		}
	}
	if (!ok)
		c = x;
	outimage(b, c, 0);
}

static void operator(Buf *b, Frame *f, Builtin *bt, Operand *ops, int n) {
	const char *name = bt->name;
	if (!strcmp(name, "[]") && n == 2) {
		bufputc(b, '{');
		vari(b, f, ops[0]);
		bufputc(b, '[');
		val(b, f, ops[1]);
		bufputs(b, "]}");
	} else if (!strcmp(name, "[:]") && n == 3) {
		bufputc(b, '{');
		vari(b, f, ops[0]);
		bufputc(b, '[');
		val(b, f, ops[1]);
		bufputc(b, ':');
		val(b, f, ops[2]);
		bufputs(b, "]}");
	} else if (!strcmp(name, "limit")) {
		bufputs(b, "limit counter: ");
		val(b, f, ops[0]);
	} else if (!strcmp(name, "[...]")) {
		bufputs(b, "[ ... ]");
	} else if (name[0] == '&') {
		bufputs(b, "bad keyword reference");
	} else if (name[0] == '.' && name[1] && n == 1) {
		bufputc(b, '{');
		val(b, f, ops[0]);
		bufputs(b, " . field}");
	} else if (!strcmp(name, "to")) {
		bufputc(b, '{');
		val(b, f, ops[0]);
		bufputs(b, " to ");
		val(b, f, ops[1]);
		bufputs(b, " by ");
		if (n > 2)
			val(b, f, ops[2]);
		else
			bufputc(b, '1');
		bufputc(b, '}');
	} else if ((!strcmp(name, "!") || !strcmp(name, "?")) && n == 1) {
		bufputc(b, '{');
		bufputs(b, name);
		vari(b, f, ops[0]);
		bufputc(b, '}');
	} else if (n == 1) {
		bufputc(b, '{');
		bufputs(b, name);
		val(b, f, ops[0]);
		bufputc(b, '}');
	} else if (n == 2) {
		bufputc(b, '{');
		converted(b, name, deref(get(f, ops[0])));
		bufprintf(b, " %s ", name);
		val(b, f, ops[1]);
		bufputc(b, '}');
	} else {
		bufputs(b, name);
		bufputc(b, '(');
		args(b, f, ops, n);
		bufputc(b, ')');
	}
}

static void offending(Buf *b, Frame *f, int pc) {
	Instr *in = &f->proc->code[pc];
	switch (in->op) {
	case OpOper:
		operator(b, f, in->fnp, in->args, in->nargs);
		return;
	case OpAssign: {
		const char *name = in->fnp ? ((Builtin *)in->fnp)->name : ":=";
		bufputc(b, '{');
		vari(b, f, in->dst);
		bufprintf(b, " %s ", name);
		if (!strcmp(name, ":=:") || !strcmp(name, "<->"))
			vari(b, f, in->x);
		else
			val(b, f, in->x);
		bufputc(b, '}');
		return;
	}
	case OpActivate:
		bufputc(b, '{');
		if (in->nargs > 0)
			val(b, f, in->args[0]);
		else
			bufputs(b, "&null");
		bufputs(b, " @ ");
		val(b, f, in->x);
		bufputc(b, '}');
		return;
	case OpScanEnter:
		bufputc(b, '{');
		val(b, f, in->x);
		bufputs(b, " ? ..}");
		return;
	case OpResume:
		if (pc > 0 && f->proc->code[pc - 1].op == OpCall && f->proc->code[pc - 1].h == in->h) {
			offending(b, f, pc - 1);
			return;
		}
		break;
	case OpCall: {
		if (in->fnp == &applyop) {
			val(b, f, in->x);
			bufputs(b, " ! ");
			val(b, f, in->args[0]);
			return;
		}
		Value fv = deref(get(f, in->x));
		if (VKIND(fv) == KBuiltin) {
			Builtin *bt = fv.w.p;
			if (bt->table == BuiltinFunction) {
				int k = in->nargs;
				int total = k;
				if (in->x.kind != ConstOp) {
					if (bt->adjmode == AdjMin && total < bt->adjn)
						total = bt->adjn;
					else if (bt->adjmode == AdjExact)
						total = bt->adjn;
				}
				bufputs(b, bt->name);
				bufputc(b, '(');
				for (int i = 0; i < total; i++) {
					if (i > 0)
						bufputc(b, ',');
					if (i < k)
						val(b, f, in->args[i]);
					else
						bufputs(b, "&null");
				}
				bufputc(b, ')');
				return;
			}
			if (bt->noderef) {
				bufputs(b, bt->name);
				bufputc(b, '(');
				args(b, f, in->args, in->nargs);
				bufputc(b, ')');
				return;
			}
			operator(b, f, bt, in->args, in->nargs);
			return;
		}
		if (VKIND(fv) == KProc) {
			bufstr(b, ((Proc *)fv.w.p)->name);
			bufputc(b, '(');
			args(b, f, in->args, in->nargs);
			bufputc(b, ')');
			return;
		}
		String s;
		if (cvstr(fv, &s)) {
			Value p = strproc(s, in->nargs);
			if (VKIND(p) != KNone) {
				if (VKIND(p) == KBuiltin)
					bufputs(b, ((Builtin *)p.w.p)->name);
				else if (VKIND(p) == KProc)
					bufstr(b, ((Proc *)p.w.p)->name);
				else
					bufstr(b, ((RecordType *)p.w.p)->name);
				bufputc(b, '(');
				args(b, f, in->args, in->nargs);
				bufputc(b, ')');
				return;
			}
		}
		outimage(b, fv, 0);
		bufputc(b, '(');
		for (int i = 0; i < in->nargs; i++) {
			if (i > 0)
				bufputc(b, ',');
			vari(b, f, in->args[i]);
		}
		bufputc(b, ')');
		return;
	}
	default:
		break;
	}
	bufputs(b, "{...}");
}

void traceback(Buf *b) {
	Frame *f = vm.cur;
	if (!f || f->pc >= f->proc->ncode)
		return;
	bufputs(b, "Traceback:\n");
	int k = 0;
	for (Frame *g = f; g; g = g->caller)
		k++;
	Frame **chain = xmalloc((size_t)k * sizeof *chain);
	k = 0;
	for (Frame *g = f; g; g = g->caller)
		chain[k++] = g;
	for (int i = k - 1; i >= 0; i--) {
		Frame *g = chain[i];
		bufstr(b, g->proc->name);
		bufputc(b, '(');
		for (int j = 0; j < g->proc->nparams; j++) {
			if (j > 0)
				bufputc(b, ',');
			outimage(b, g->locals[j], 0);
		}
		bufputc(b, ')');
		calledfrom(b, g);
		bufputc(b, '\n');
	}
	free(chain);
	offending(b, f, f->pc);
	size_t n;
	const char *file = fileat(f->proc, f->pc, &n);
	bufprintf(b, " from line %d in ", f->proc->code[f->pc].line);
	bufadd(b, file, n);
	bufputc(b, '\n');
}

/* ---- Names ---- */

void varname(Buf *b, Value v) {
	switch (VKIND(v)) {
	case KSubstr: {
		Substr *s = v.w.p;
		varname(b, s->v);
		bufprintf(b, "[%lld:%lld]", (long long)s->pos, (long long)(s->pos + s->n));
		return;
	}
	case KListElem:
		bufprintf(b, "L[%lld]", (long long)VN(v) + 1);
		return;
	case KField: {
		Record *r = v.w.p;
		bufstr(b, r->t->name);
		bufputc(b, '.');
		bufstr(b, r->t->fields[VN(v)]);
		return;
	}
	case KVar: {
		Var *x = v.w.p;
		switch (x->kind) {
		case VTableElem:
			bufputs(b, "T[");
			image(b, x->k);
			bufputc(b, ']');
			return;
		case VPos: bufputs(b, "&pos"); return;
		case VSubject: bufputs(b, "&subject"); return;
		case VRandom: bufputs(b, "&random"); return;
		case VTrace: bufputs(b, "&trace"); return;
		case VError: bufputs(b, "&error"); return;
		}
		break;
	}
	case KRef: {
		Value *p = v.w.p;
		Program *pr = vm.prog;
		if (p >= vm.globals && p < vm.globals + pr->nglobals) {
			Str nm = pr->globalnames[p - vm.globals];
			const char *sp = memchr(nm.s, ' ', nm.n);
			if (sp)
				bufadd(b, sp + 1, nm.n - (size_t)(sp + 1 - nm.s));
			else
				bufstr(b, nm);
			return;
		}
		Frame *f = vm.cur;
		if (f && p >= f->locals && p < f->locals + f->proc->nlocals && p - f->locals < f->proc->nlocalnames) {
			bufstr(b, f->proc->localnames[p - f->locals]);
			return;
		}
		break;
	}
	}
	bufputc(b, '?');
}

static int cmpglobal(const void *a, const void *b) {
	Str x = vm.prog->globalnames[*(const int *)a], y = vm.prog->globalnames[*(const int *)b];
	int c = memcmp(x.s, y.s, x.n < y.n ? x.n : y.n);
	return c ? c : (x.n > y.n) - (x.n < y.n);
}

void listing(Buf *b, int64_t n) {
	Program *p = vm.prog;
	for (Frame *fr = vm.cur; fr && n > 0; fr = fr->caller, n--) {
		Proc *pr = fr->proc;
		bufstr(b, pr->name);
		bufputs(b, " local identifiers:\n");
		for (int i = 0; i < pr->nlocalnames; i++) {
			bufputs(b, "   ");
			bufstr(b, pr->localnames[i]);
			bufputs(b, " = ");
			outimage(b, fr->locals[i], 0);
			bufputc(b, '\n');
		}
		for (int i = 0; i < pr->nstatics; i++) {
			Str nm = p->globalnames[pr->statics[i]];
			const char *sp = memchr(nm.s, ' ', nm.n);
			bufputs(b, "   ");
			bufadd(b, sp + 1, nm.n - (size_t)(sp + 1 - nm.s));
			bufputs(b, " = ");
			outimage(b, vm.globals[pr->statics[i]], 0);
			bufputc(b, '\n');
		}
	}
	bufputs(b, "\nglobal identifiers:\n");
	int *gs = xmalloc((size_t)(p->nglobals + 1) * sizeof(int));
	int k = 0;
	for (int i = 0; i < p->nglobals; i++)
		if (!memchr(p->globalnames[i].s, ' ', p->globalnames[i].n) && (!p->invocable || p->invocable[i]))
			gs[k++] = i;
	qsort(gs, (size_t)k, sizeof(int), cmpglobal);
	for (int i = 0; i < k; i++) {
		bufputs(b, "   ");
		bufstr(b, p->globalnames[gs[i]]);
		bufputs(b, " = ");
		outimage(b, vm.globals[gs[i]], 0);
		bufputc(b, '\n');
	}
	free(gs);
}

void termdump(void) {
	if (vm.dump == 0)
		return;
	flush_output();
	Buf b = {0};
	bufprintf(&b, "\nTermination dump:\n\nco-expression #%lld(%lld)\n", (long long)vm.co->serial,
		  (long long)vm.co->results);
	listing(&b, vm.level);
	emit(vm.errfile, b.b, b.n, true);
}
