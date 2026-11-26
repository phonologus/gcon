/*
 * gcon vet: as Go's vet, it reports what is almost certainly a mistake
 * (docs/design.md, "gcon vet"). The first checks find code that fails
 * at run time:
 *
 *   - a field reference x.name where no record has a field name
 *     (run-time error 207);
 *   - a numeric comparison with a literal that is not a number (102);
 *   - a call of a local or static that shadows a procedure, record
 *     constructor or built-in function and is never assigned (106);
 *   - string invocation ("name"(...)) of a procedure or record the
 *     linker discards, not being reached from main or declared
 *     invocable (106).
 *
 * and, if asked (icon95), what is not Icon 9.5: gcon's \u and \U
 * escapes and cset ranges, and its built-ins and keywords (not file
 * mode "u", which is 9.5's too). The checks of the whole program
 * (fields, string invocation) are made only for a complete program
 * that declares main.
 */
#ifndef GCON_VET_H
#define GCON_VET_H

#include "sema.h"

typedef struct VetFinding {
	SemaFile *file;
	size_t offset, end; /* byte span in file->syn */
	char *msg;
} VetFinding;
typedef VEC(VetFinding) VetFindings;

typedef struct VetOptions {
	bool icon95; /* also report what is not Icon 9.5 */
} VetOptions;

/* VetResolver returns the resolution of a file of the program; vet
 * asks for files other than the one it checks only when it needs to
 * know which procedures the linker keeps. */
typedef SemaInfo *VetResolver(void *ctx, SemaFile *f);

/* vet_file appends the findings in file f (resolved as info) of
 * program p to out, in source order. */
void vet_file(Program *p, SemaInfo *info, VetResolver *resolve, void *ctx, VetOptions opts, VetFindings *out);

#endif
