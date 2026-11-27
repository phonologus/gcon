/*
 * The built program's format (gcon's docs/design.md, "The built
 * program's format"; the Go reference's internal/vm/encode.go and
 * instr.go): what gcon-vm's loader reads and the front end's encoder
 * (../../src/encode.c) writes, defined once for both.
 */
#ifndef GCON_FORMAT_H
#define GCON_FORMAT_H

/* The version of the format. It changes with any change to the
 * encoding, to the instruction set, or to what an instruction does with
 * its fields. */
enum { FormatVersion = 2 };

/* Magic begins an encoded program. As in PNG, the first byte is not
 * ASCII and the line endings catch a file mangled as text. */
#define FORMAT_MAGIC "\x89GCN\r\n\x1a\n"

/* Section tags: upper case required, lower case skippable; "End " last,
 * holding the CRC-32 of everything before it. */
#define TAG_BUILTINS "Bltn"
#define TAG_RECORDS "Recd"
#define TAG_PROCS "Proc"
#define TAG_CONSTS "Cnst"
#define TAG_GLOBALS "Glob"
#define TAG_INFO "info"
#define TAG_END "End "

enum OperandKind { NoOperand, TempOp, LocalOp, GlobalOp, ConstOp };

enum Op {
	OpGoto, OpIGoto, OpSetLabel, OpMove, OpMoveVar, OpAssign, OpOper,
	OpCall, OpResume, OpReturn, OpFail, OpSuspend, OpOnce, OpIsNull,
	OpCreate, OpActivate, OpCoRet, OpCoFail, OpScanEnter, OpScanSwap,
	NumOps
};

/* Which table a builtin comes from; with its name, it identifies it. */
enum BuiltinKind {
	BuiltinOp, BuiltinFunction, BuiltinAsgn, BuiltinApply, BuiltinField,
	BuiltinUnsupported, NumBuiltinKinds
};

/* Value tags. */
enum {
	ValNull, ValInt, ValBig, ValReal, ValString, ValCset, ValBuiltin, ValProc, ValRecord
};

/* Instruction field flags: a field is present when nonzero (an operand
 * when it has a kind, the arguments when there are some). */
enum {
	InstrDst = 1 << 0,
	InstrX = 1 << 1,
	InstrH = 1 << 2,
	InstrArgs = 1 << 3,
	InstrFn = 1 << 4,
	InstrL1 = 1 << 5,
	InstrL2 = 1 << 6,
	InstrFile = 1 << 7,
};

#endif
