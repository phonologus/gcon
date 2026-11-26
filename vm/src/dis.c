/*
 * The listing of a program, identical to gcon's (internal/vm/disasm.go,
 * `gcon dis`), so that the two can be compared: the test that the
 * loader reads what gcon wrote.
 */
#include "gcon-vm.h"

#include <stdlib.h>
#include <string.h>

const char *const opnames[NumOps] = {
	"goto", "igoto", "setlabel", "move", "movevar", "assign", "oper", "call",
	"resume", "return", "fail", "suspend", "once", "isnull", "create",
	"activate", "coret", "cofail", "scanenter", "scanswap"
};

static void operand(Buf *b, Operand o) {
	static const char kinds[] = "-tlgk";
	if (o.kind == NoOperand || o.kind > ConstOp)
		bufputc(b, '-');
	else
		bufprintf(b, "%c%d", kinds[o.kind], o.index);
}

/* padded writes s left-justified in width runes, as Go's %-*s. */
static void padded(Buf *b, Str s, int width) {
	bufstr(b, s);
	int n = 0;
	for (size_t i = 0; i < s.n;) {
		int w;
		utf8decode((const uint8_t *)s.s + i, s.n - i, &w);
		i += (size_t)w;
		n++;
	}
	for (; n < width; n++)
		bufputc(b, ' ');
}

static void instr(Buf *b, const Program *p, const Instr *in) {
	bufputs(b, opnames[in->op]);
	if (in->fn >= 0) {
		bufputs(b, " [");
		builtinref(b, &p->builtins[in->fn]);
		bufputc(b, ']');
	}
	if (in->dst.kind != NoOperand) {
		bufputc(b, ' ');
		operand(b, in->dst);
		bufputs(b, " :=");
	}
	if (in->x.kind != NoOperand) {
		bufputc(b, ' ');
		operand(b, in->x);
	}
	for (int i = 0; i < in->nargs; i++) {
		bufputc(b, ' ');
		operand(b, in->args[i]);
	}
	switch (in->op) {
	case OpGoto: case OpSetLabel: case OpOnce: case OpSuspend: case OpCreate: case OpCoRet:
		bufprintf(b, " ->%d", in->l1);
		if (in->l2 != 0)
			bufprintf(b, " L2=%d", in->l2);
		break;
	case OpOper: case OpCall: case OpResume: case OpIsNull: case OpActivate: case OpAssign:
		bufprintf(b, " ->%d/%d", in->l1, in->l2);
		break;
	case OpScanEnter:
		bufprintf(b, " fail->%d", in->l2);
		if (in->l1 != 0)
			bufprintf(b, " L1=%d", in->l1);
		break;
	default:
		if (in->l1 != 0)
			bufprintf(b, " L1=%d", in->l1);
		if (in->l2 != 0)
			bufprintf(b, " L2=%d", in->l2);
	}
	if (in->h != 0 || in->op == OpCall || in->op == OpResume)
		bufprintf(b, " h=t%d", in->h);
	if (in->file != 0)
		bufprintf(b, " file=%d", in->file);
}

static void proc(Buf *b, const Program *p, const Proc *pr) {
	bufputs(b, "\nprocedure ");
	bufstr(b, pr->name);
	bufputs(b, " (");
	bufstr(b, pr->file);
	bufputs(b, ")\n");
	bufprintf(b, "  params %d%s; locals %d", pr->nparams, pr->variadic ? " variadic" : "",
		  pr->nlocals);
	if (pr->nlocalnames > 0) {
		bufputs(b, " (");
		for (int i = 0; i < pr->nlocalnames; i++) {
			if (i > 0)
				bufputc(b, ' ');
			bufstr(b, pr->localnames[i]);
		}
		bufputc(b, ')');
	}
	bufprintf(b, "; temps %d", pr->ntemps);
	if (pr->nstatics > 0) {
		bufputs(b, "; statics");
		for (int i = 0; i < pr->nstatics; i++)
			bufprintf(b, " g%d", pr->statics[i]);
	}
	bufputc(b, '\n');
	for (int i = 0; i < pr->nfiles; i++) {
		bufprintf(b, "  file %d: ", i + 1);
		bufstr(b, pr->files[i]);
		bufputc(b, '\n');
	}
	for (int pc = 0; pc < pr->ncode; pc++) {
		bufprintf(b, "  %5d %5d  ", pc, pr->code[pc].line);
		instr(b, p, &pr->code[pc]);
		bufputc(b, '\n');
	}
}

void disasm(Buf *b, const Program *p) {
	bufprintf(b, "format %d\n", FormatVersion);
	for (int i = 0; i < p->ninfo; i++) {
		bufputs(b, "info ");
		bufstr(b, p->infokeys[i]);
		bufputs(b, ": ");
		bufstr(b, p->infovals[i]);
		bufputc(b, '\n');
	}
	bufputs(b, "\nconstants\n");
	for (int i = 0; i < p->nconsts; i++) {
		bufprintf(b, "  k%-4d ", i);
		constimage(b, p, p->consts[i]);
		bufputc(b, '\n');
	}
	bufputs(b, "\nglobals\n");
	for (int i = 0; i < p->nglobals; i++) {
		bufprintf(b, "  g%-4d ", i);
		padded(b, p->globalnames[i], 16);
		bufputc(b, ' ');
		constimage(b, p, p->globals[i]);
		bufputc(b, '\n');
	}
	if (p->main >= 0)
		bufprintf(b, "main g%lld\n", (long long)p->main);
	else
		bufputs(b, "no main\n");
	if (p->invocable == NULL)
		bufputs(b, "invocable all\n");
	else {
		bufputs(b, "invocable");
		for (int i = 0; i < p->nglobals; i++)
			if (p->invocable[i])
				bufprintf(b, " g%d", i);
		bufputc(b, '\n');
	}
	bool *seen = xcalloc((size_t)p->nprocs, sizeof *seen);
	for (int i = 0; i < p->nglobals; i++) {
		if (VKIND(p->globals[i]) != KProc)
			continue;
		const Proc *pr = p->globals[i].w.p;
		if (!seen[pr - p->procs]) {
			seen[pr - p->procs] = true;
			proc(b, p, pr);
		}
	}
	free(seen);
}
