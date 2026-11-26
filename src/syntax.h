/*
 * The parser (the Go reference's internal/syntax): Icon source to a
 * lossless syntax tree.
 *
 * The tree is typed, one Node struct with a union per kind, and keeps
 * every token (with its comments and whitespace as leading trivia), so
 * the source can be reproduced exactly; syn_children visits a node's
 * tokens and child nodes in source order, each token of the file once.
 * Nothing here recurses, so nesting has no ceiling but memory; passes
 * over the tree should walk it iteratively too.
 * syn_parse never fails: errors are collected in File.errs, and
 * recovery produces bad-declaration and bad-expression nodes and NULL
 * fields for missing parts, so an incomplete file (as a language server
 * sees it) still yields a useful tree.
 *
 * The grammar follows icon/src/h/grammar.h: precedence and
 * associativity follow its expr..expr11 levels, and control structures
 * (if, while, every, return, create, ...) extend as far to the right
 * as possible, as yacc's shift preference makes them do in icont.
 */
#ifndef GCON_SYNTAX_H
#define GCON_SYNTAX_H

#include "lexer.h"

typedef enum NodeKind {
	/* declarations */
	N_LINK, N_GLOBAL, N_RECORD, N_INVOCABLE, N_INVITEM, N_PROC, N_LOCAL,
	N_INITIAL, N_BADDECL,
	/* expressions */
	N_IDENT, N_LITERAL, N_KEYWORD, N_UNARY, N_BINARY, N_TOBY, N_PAREN,
	N_BLOCK, N_LISTLIT, N_INDEX, N_SECTION, N_CALL, N_PDCO, N_FIELD,
	N_CREATE, N_IF, N_LOOP, N_RETURN, N_BREAK, N_NEXT, N_CASE,
	N_CASECLAUSE, N_BADEXPR,
	N_NKINDS
} NodeKind;

const char *node_name(NodeKind k); /* as the Go type name: "ProcDecl", ... */

typedef struct Node Node;
typedef VEC(Token *) TokVec;
typedef VEC(Node *) NodeVec;

/* A sequence with separators (commas or semicolons): seps.v[i] follows
 * elems.v[i]; there is either one separator fewer than elements or, for
 * semicolon-terminated sequences, the same number. Elements may be NULL
 * where the grammar allows an empty expression, as in f(a,,b), and a
 * separator may be NULL where one was missing (a procedure body). A
 * list of a single empty element and no separators, such as the
 * arguments of f(), has no elements at all. */
typedef struct TokList {
	TokVec elems, seps;
} TokList;
typedef struct NodeList {
	NodeVec elems;
	TokVec seps;
} NodeList;

/* An element of a bad declaration or expression: a token or a node. */
typedef struct Elem {
	Token *tok;
	Node *node;
} Elem;
typedef VEC(Elem) ElemVec;

struct Node {
	NodeKind kind;
	union {
		/* link a, "b" */
		struct { Token *link; TokList files; } link;
		/* global a, b */
		struct { Token *global; TokList names; } global;
		/* record name(field, ...) */
		struct { Token *record, *name, *lparen; TokList fields; Token *rparen; } record;
		/* invocable name, "op":arity, ...: items are N_INVITEM */
		struct { Token *invocable; NodeList items; } invocable;
		/* name | "string" | "string":arity */
		struct { Token *name, *colon, *arity; } invitem;
		/* procedure name(params[]) locals initial body end; body's
		 * seps are semicolons, one per element (usually implicit) */
		struct {
			Token *procedure, *name, *lparen;
			TokList params;
			Token *lbrack, *rbrack, *rparen, *semi;
			NodeVec locals; /* N_LOCAL */
			Node *initial;  /* N_INITIAL, or NULL */
			NodeList body;
			Token *end;
		} proc;
		/* local a, b;  or  static a, b; */
		struct { Token *keyword; TokList names; Token *semi; } local;
		/* initial x; */
		struct { Token *initial; Node *x; Token *semi; } initial;
		/* N_BADDECL, N_BADEXPR: what could not be parsed */
		struct { ElemVec elems; } bad;

		struct { Token *name; } ident;
		struct { Token *tok; } literal;
		/* &name (name may be the reserved word fail) */
		struct { Token *amp, *name; } keyword;
		/* a prefix operator, including "not" */
		struct { Token *op; Node *x; } unary;
		/* an infix operator, including &, ?, |, :=, augmented
		 * assignments, limitation, activation and application */
		struct { Node *x; Token *op; Node *y; } binary;
		/* x to y [by z] */
		struct { Node *x; Token *to; Node *y; Token *by; Node *z; } toby;
		/* N_PAREN (e1, ...), N_BLOCK {e1; ...}, N_LISTLIT [e1, ...] */
		struct { Token *open; NodeList list; Token *close; } group;
		/* x[i, j, ...] */
		struct { Node *x; Token *lbrack; NodeList list; Token *rbrack; } index;
		/* x[lo:hi], x[lo+:n], x[lo-:n] */
		struct { Node *x; Token *lbrack; Node *lo; Token *op; Node *hi; Token *rbrack; } section;
		/* N_CALL f(args), N_PDCO f{args} */
		struct { Node *fun; Token *open; NodeList args; Token *close; } call;
		/* x.name */
		struct { Node *x; Token *dot, *name; } field;
		struct { Token *create; Node *x; } create;
		/* if cond then body [else alt] */
		struct { Token *if_; Node *cond; Token *then; Node *body; Token *else_; Node *alt; } if_;
		/* while/until/every cond [do body], repeat body (cond NULL) */
		struct { Token *keyword; Node *cond; Token *do_; Node *body; } loop;
		/* fail, return [x], suspend [x] [do body] */
		struct { Token *keyword; Node *x; Token *do_; Node *body; } ret;
		struct { Token *break_; Node *x; } break_;
		struct { Token *next; } next;
		/* case x of { clauses }: clauses are N_CASECLAUSE, seps
		 * semicolons */
		struct { Token *case_; Node *x; Token *of, *lbrace; NodeList clauses; Token *rbrace; } case_;
		/* label : body, or default : body */
		struct { Token *default_; Node *label; Token *colon; Node *body; } clause;
	};
};

/* A lexical or syntax error. */
typedef struct SynError {
	size_t offset;
	const char *msg;
} SynError;

typedef struct File {
	const char *src;
	size_t n;
	Tokens toks;   /* all tokens, including the final T_EOF */
	NodeVec decls;
	Token *eof;    /* carries trailing trivia */
	VEC(SynError) errs;
} File;

/* syn_parse parses an Icon source file (n bytes). The tokens cover the
 * whole source; problems are reported in errs, the lexer's first. */
File *syn_parse(const char *src, size_t n);

/* syn_children calls f for each child of n, a token or a node (the
 * other NULL), in source order; absent children are skipped.
 * syn_file_children does the same for the file's declarations and
 * final EOF token. */
typedef void SynVisit(void *ctx, Token *t, Node *n);
void syn_children(Node *n, SynVisit *f, void *ctx);
void syn_file_children(File *file, SynVisit *f, void *ctx);

/* syn_tokens calls f for every token of the file, in source order (the
 * Go's syntax.Tokens), without recursion. */
void syn_tokens(File *file, void (*f)(void *ctx, Token *t), void *ctx);

#endif
