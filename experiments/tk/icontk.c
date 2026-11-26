/*
 * icontk.c: a Tcl/Tk binding for gcon, loaded by loadfunc (experiment).
 *
 * Written to Icon 9.5's icall.h, so it needs nothing of gcon-vm's but
 * that header, and builds against iconx too. Tcl's interface is strings,
 * which is what icall.h passes well; everything Tk can do is reached by
 * evaluating Tcl.
 *
 * icall.h gives C no way to call Icon, so Tk cannot call an Icon
 * procedure. Instead the binding defines one Tcl command, "icon": a
 * widget's callback runs it ("icon quit", "icon click %x %y"), which
 * queues its arguments as an event; the program takes events with
 * tk_event(), as it takes Icon's own window events with Event().
 *
 *   tk_init()           start Tcl and Tk (once; later calls do nothing)
 *   tk_eval(s)          evaluate Tcl script s, giving its result; a Tcl
 *                       error is run-time error 500, the message its value
 *   tk_event([msec])    the name of the next event ("icon" command's first
 *                       argument), handling Tk's events until one comes:
 *                       for ever, or at most msec milliseconds (0: only
 *                       what is pending), failing if none came; it fails
 *                       too once every Tk window is gone. The event's
 *                       words are in the Tcl variable icon_event.
 *
 * Closing the main window is the event "close" (wm protocol).
 *
 * Copyright (c) 2026 Sean Jensen. MIT licence.
 */
#include <string.h>
#include <tcl.h>
#include <tk.h>

#include "icall.h"

static Tcl_Interp *interp;

/* The queued events: Tcl lists, oldest first. */
static Tcl_Obj **queue;
static int qhead, qlen, qcap;

static void push(Tcl_Obj *ev) {
	if (qlen == qcap) {
		int cap = qcap ? 2 * qcap : 16;
		Tcl_Obj **q = (Tcl_Obj **)Tcl_Alloc(cap * sizeof *q);
		for (int i = 0; i < qlen; i++)
			q[i] = queue[(qhead + i) % qcap];
		if (queue)
			Tcl_Free((char *)queue);
		queue = q, qcap = cap, qhead = 0;
	}
	Tcl_IncrRefCount(ev);
	queue[(qhead + qlen++) % qcap] = ev;
}

static Tcl_Obj *pop(void) {
	if (qlen == 0)
		return NULL;
	Tcl_Obj *ev = queue[qhead];
	qhead = (qhead + 1) % qcap;
	qlen--;
	return ev;
}

/* The Tcl command "icon word ...": queue the words as an event. */
static int iconcmd(void *cd, Tcl_Interp *ip, Tcl_Size objc, Tcl_Obj *const objv[]) {
	(void)cd;
	if (objc < 2) {
		Tcl_WrongNumArgs(ip, 1, objv, "event ?arg ...?");
		return TCL_ERROR;
	}
	push(Tcl_NewListObj(objc - 1, objv + 1));
	return TCL_OK;
}

/* tclerror makes the interpreter's result Icon run-time error 500. */
static int tclerror(descriptor *argv) {
	Tcl_Size n;
	const char *s = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &n);
	argv[0].dword = n;
	argv[0].vword = (word)alcstr((char *)s, n);
	return 500;
}

int tk_init(int argc, descriptor argv[]) /*: start Tcl and Tk */
{
	(void)argc;
	if (interp)
		Return;
	Tcl_FindExecutable(NULL);
	interp = Tcl_CreateInterp();
	if (Tcl_Init(interp) != TCL_OK || Tk_Init(interp) != TCL_OK)
		return tclerror(argv);
	Tcl_CreateObjCommand2(interp, "icon", iconcmd, NULL, NULL);
	if (Tcl_Eval(interp, "wm protocol . WM_DELETE_WINDOW {icon close}") != TCL_OK)
		return tclerror(argv);
	Return;
}

int tk_eval(int argc, descriptor argv[]) /*: evaluate Tcl script */
{
	ArgString(1);
	if (!interp)
		Error(500);
	if (Tcl_EvalEx(interp, StringAddr(argv[1]), StringLen(argv[1]), TCL_EVAL_GLOBAL) != TCL_OK)
		return tclerror(argv);
	Tcl_Size n;
	const char *s = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &n);
	RetStringN((char *)s, n);
}

static void timeout(void *cd) { *(int *)cd = 1; }

int tk_event(int argc, descriptor argv[]) /*: next event from Tk */
{
	long msec = -1;
	if (argc >= 1 && IconType(argv[1]) != 'n') {
		ArgInteger(1);
		msec = IntegerVal(argv[1]);
	}
	if (!interp)
		Error(500);
	int expired = 0;
	Tcl_TimerToken t = NULL;
	if (msec > 0)
		t = Tcl_CreateTimerHandler((int)msec, timeout, &expired);
	while (qlen == 0 && !expired && Tk_GetNumMainWindows() > 0) {
		if (msec == 0) {
			if (!Tcl_DoOneEvent(TCL_ALL_EVENTS | TCL_DONT_WAIT))
				break;
		} else
			Tcl_DoOneEvent(TCL_ALL_EVENTS);
	}
	if (t && !expired)
		Tcl_DeleteTimerHandler(t);
	Tcl_Obj *ev = pop();
	if (!ev)
		Fail;
	Tcl_SetVar2Ex(interp, "icon_event", NULL, ev, TCL_GLOBAL_ONLY);
	Tcl_Obj *name;
	Tcl_ListObjIndex(interp, ev, 0, &name);
	Tcl_Size n;
	const char *s = Tcl_GetStringFromObj(name, &n);
	argv[0].dword = n;
	argv[0].vword = (word)alcstr((char *)s, n);
	Tcl_DecrRefCount(ev);
	Return;
}
