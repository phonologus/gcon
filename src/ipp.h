/*
 * The Icon preprocessor: icont's (icon/src/common/ipp.c), whose code
 * this is, adapted to give gcon's form of output (the Go reference's
 * internal/ipp).
 *
 * It recognises, on lines whose first non-blank character is $ (not
 * followed by punctuation other than a space, so that $( and the other
 * digraphs are left alone):
 *
 *	$define name text    $undef name
 *	$ifdef name          $ifndef name     $else     $endif
 *	$include filename    $line n [filename]          $error [text]
 *
 * and #line n [filename] at the start of a line. Each identifier
 * outside string and cset literals, numbers and comments that names a
 * definition is replaced by its text, which is scanned again (a
 * definition is not expanded within itself).
 *
 * Where icont drops directive and skipped lines and writes #line
 * markers, this writes an empty line for each, so the output has one
 * line for each line read, and a map records, for each output line, the
 * file and line it came from: how positions survive $include.
 */
#ifndef GCON_IPP_H
#define GCON_IPP_H

#include "util.h"

typedef struct PPOptions {
	/* the symbols defined before the file is read (icont defines its
	 * features, each as "1"): names and values, npredef of each */
	const char *const *prenames, *const *prevalues;
	size_t npredef;
	/* the directories $include searches after the current directory
	 * (icont's LPATH, split) */
	const char *const *incpath;
	size_t nincpath;
} PPOptions;

/* An error, as icont reports them: "File %s; Line %d # %s". */
typedef struct PPError {
	const char *file;
	long line;
	const char *msg;
} PPError;

typedef struct PPPos {
	const char *file;
	long line;
} PPPos;

/* The map from output lines to where they came from: starts[i] is the
 * byte offset of output line i, pos[i] its source. */
typedef struct PPMap {
	VEC(size_t) starts;
	VEC(PPPos) pos;
} PPMap;

typedef struct PPResult {
	Buf src; /* the output */
	PPMap map;
	VEC(PPError) errs;
} PPResult;

/* ipp_preprocess preprocesses src (n bytes), the contents of the file
 * name. */
PPResult *ipp_preprocess(const char *name, const char *src, size_t n, const PPOptions *opts);

/* ipp_locate gives the file and line of byte offset off in the output. */
void ipp_locate(const PPMap *m, size_t off, const char **file, long *line);

/* ipp_error formats an error as icont does (malloc'd). */
char *ipp_error(const PPError *e);

#endif
