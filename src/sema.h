/*
 * Name resolution (the Go reference's internal/sema): the identifiers
 * of Icon programs resolved to their declarations, by icont's rules,
 * with icont's diagnostics (its translator's tsym.c and linker's
 * lglob.c and lsym.c).
 *
 * A Program is a set of files linked together. Within a procedure, an
 * identifier refers to (in order): a parameter, local or static of the
 * procedure; a global (declared by global, procedure or record in any
 * file of the program); a built-in function; otherwise it is an
 * implicit local of the procedure ("undeclared identifier", which icont
 * -u warns about). A field name in x.name may belong to any record
 * declaring it.
 *
 * Nothing here recurses: expressions are walked with an explicit stack.
 */
#ifndef GCON_SEMA_H
#define GCON_SEMA_H

#include "builtin.h"
#include "syntax.h"

typedef enum SymKind {
	S_PARAM,     /* procedure parameter */
	S_LOCAL,     /* local declaration */
	S_STATIC,    /* static declaration */
	S_IMPLICIT,  /* undeclared, hence local to its procedure */
	S_GLOBAL,    /* global declaration */
	S_PROCEDURE, /* procedure declaration */
	S_RECORD,    /* record declaration */
	S_FIELD,     /* field of a record declaration */
	S_BUILTIN,   /* built-in function */
	S_MACRO,     /* preprocessor symbol ($define, or predefined) */
	S_NKINDS
} SymKind;

const char *sym_kind_name(SymKind k); /* "parameter", "local", ... */
static inline bool sym_is_local(SymKind k) { return k <= S_IMPLICIT; }

typedef struct SemaFile SemaFile;

/* A preprocessor symbol definition. */
typedef struct MacroDef {
	const char *name, *value;
	const char *path;    /* file holding the $define ("" if predefined) */
	size_t offset, end;  /* byte span of the name in that file */
	const Predefined *predefined;
} MacroDef;

/* A macro definition in force in part of a file: from byte offset from
 * (the $define, or the $include that brought it in) to until (a $undef
 * or redefinition, or the end of the file). */
typedef struct MacroScope {
	MacroDef *def;
	size_t from, until;
} MacroScope;

/* A declared (or implicitly declared) name. */
typedef struct Symbol {
	const char *name;
	SymKind kind;
	SemaFile *file; /* declaring file (NULL for built-ins and macros) */
	Token *decl;    /* declaring token (for S_IMPLICIT, the first use) */
	/* The declaration: the N_PROC for procedures and for local kinds
	 * (the enclosing procedure), the N_RECORD for records and fields,
	 * the N_GLOBAL for globals. */
	Node *node;
	const BuiltinFunc *builtin; /* for S_BUILTIN */
	MacroDef *macro;            /* for S_MACRO */
} Symbol;

typedef VEC(Symbol *) SymVec;

struct SemaFile {
	const char *path;
	File *syn;
	/* preprocessor symbols defined in or included by the file */
	MacroScope *macros;
	size_t nmacros;
	/* set when syn is the preprocessor's output, in which no identifier
	 * is a preprocessor symbol any longer */
	bool preprocessed;
};

typedef struct Program {
	SemaFile **files;
	size_t nfiles;
	StrMap globals; /* name -> SymVec *: procedures, records and globals */
	StrMap fields;  /* name -> SymVec *: record fields */
	/* false if some linked files could not be found, so that
	 * unresolved names may be declared there; undeclared identifiers
	 * are then not reported */
	bool complete;
} Program;

typedef enum Severity { SEV_ERROR, SEV_WARNING } Severity;

/* A semantic problem in a file. msg is icont's text where icont has the
 * diagnostic; related, if set, is the other declaration a redeclaration
 * conflicts with. */
typedef struct Diagnostic {
	size_t offset, end;
	Severity severity;
	const char *msg;
	Symbol *related;
} Diagnostic;

/* A procedure's local symbols, in the order they were declared or
 * first used. */
typedef struct ProcLocals {
	Node *proc;
	StrMap byname; /* name -> Symbol * */
	SymVec syms;
} ProcLocals;

/* The resolution of one file of a program. The per-token arrays are
 * indexed by the token's index in file->syn->toks. */
typedef struct SemaInfo {
	SemaFile *file;
	/* each identifier token (in expressions, and the names in
	 * declarations) to its symbol */
	Symbol **uses;
	/* the name token of each field reference x.name to the fields it
	 * may denote (an empty vector if no record has such a field; NULL
	 * for other tokens) */
	SymVec **fielduses;
	/* the name token of each &keyword to the keyword (NULL if there is
	 * no such keyword); iskeyword marks those tokens */
	const Keyword **keywords;
	bool *iskeyword;
	VEC(ProcLocals *) locals; /* one for each procedure, in order */
	VEC(Diagnostic) diags;    /* sorted by offset */
} SemaInfo;

/* sema_program collects the global declarations of files. */
Program *sema_program(SemaFile **files, size_t n, bool complete);

/* sema_global returns the symbol a global name refers to: preferably a
 * procedure or record (whose declarations make the global), else the
 * first global declaration, else a built-in function; or NULL. */
Symbol *sema_global(Program *p, const char *name, size_t n);

/* sema_resolve resolves the identifiers of f, one of p's files. */
SemaInfo *sema_resolve(Program *p, SemaFile *f);

/* sema_locals returns the locals of procedure proc, or NULL. */
ProcLocals *sema_locals(SemaInfo *info, Node *proc);

#endif
