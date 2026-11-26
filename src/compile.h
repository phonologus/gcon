/*
 * The compiler (the Go reference's internal/compile): resolved syntax
 * trees to a built program (prog.h), by Proebsting's translation of
 * goal-directed evaluation, without recursion (compile.c).
 */
#ifndef GCON_COMPILE_H
#define GCON_COMPILE_H

#include "ipp.h"
#include "prog.h"
#include "sema.h"

/* A file of the program: its resolution, and the preprocessor's map
 * of where its lines came from (NULL: the lines of file->path). */
typedef struct CompileFile {
	SemaInfo *info;
	const PPMap *map;
} CompileFile;

/* compile_program compiles a program whose files (n, in the order of
 * p's) have been resolved. Errors, as "File F; Line N # msg", are added
 * to errs; the program is returned even if there are some. */
Prog *compile_program(Program *p, CompileFile *files, size_t n, StrVec *errs);

#endif
