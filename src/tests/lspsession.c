/*
 * lspsession: sessions for the language server's tests (lsp.sh).
 *
 *   lspsession basic FILE HIER [IPATHDIR ...]
 *	a session: initialize (hierarchical document symbols if HIER is
 *	1; the IPATHDIRs as the client's ipath), open FILE, its document
 *	symbols and formatting, shutdown, exit
 *   lspsession features FILE STEP [IPATHDIR ...]
 *	as basic, then for every STEPth identifier or keyword name: hover,
 *	definition, references (with and without declarations), highlights,
 *	signature help, completion, prepare rename and rename (to "zzz");
 *	then workspace symbols ($LSPCOMPLETE=n: completion at every nth of
 *	those positions only)
 *   ($LSPICON95 set: the client asks for vet's Icon 9.5 checks)
 *   lspsession split
 *	a filter: a server's output, each message's JSON on a line
 *   lspsession diags
 *	a filter: the diagnostics in a server's output, as gcon build reports
 *	errors ("LINE # MESSAGE")
 */
#include "../json.h"
#include "../lexer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Buf out;
static int nextid = 1;

static void msg(const char *json) {
	printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
}

static void request(const char *method, const char *params) {
	Buf b = {0};
	bufprintf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}", nextid++, method, params);
	msg(b.b);
}

static void notification(const char *method, const char *params) {
	Buf b = {0};
	bufprintf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);
	msg(b.b);
}

static int split(void) {
	char line[4096];
	for (;;) {
		long len = -1;
		bool any = false;
		while (fgets(line, sizeof line, stdin)) {
			any = true;
			if (!strcmp(line, "\r\n") || !strcmp(line, "\n"))
				break;
			if (!strncasecmp(line, "Content-Length:", 15))
				len = strtol(line + 15, NULL, 10);
		}
		if (!any || len < 0)
			return 0;
		char *b = malloc((size_t)len + 1);
		if (fread(b, 1, (size_t)len, stdin) != (size_t)len)
			return 1;
		fwrite(b, 1, (size_t)len, stdout);
		putchar('\n');
		free(b);
	}
}

/* diags: the diagnostics a server published, one a line, as gcon build
 * reports errors: "LINE # MESSAGE" (LINE from 1). */
static int diags(void) {
	char line[4096];
	for (;;) {
		long len = -1;
		bool any = false;
		while (fgets(line, sizeof line, stdin)) {
			any = true;
			if (!strcmp(line, "\r\n") || !strcmp(line, "\n"))
				break;
			if (!strncasecmp(line, "Content-Length:", 15))
				len = strtol(line + 15, NULL, 10);
		}
		if (!any || len < 0)
			return 0;
		char *b = malloc((size_t)len + 1);
		if (fread(b, 1, (size_t)len, stdin) != (size_t)len)
			return 1;
		const char *err;
		JVal *v = json_parse(b, (size_t)len, &err);
		if (v && !strcmp(json_str(v, "method"), "textDocument/publishDiagnostics")) {
			JVal *ds = json_get(v, "params.diagnostics");
			for (size_t i = 0; ds && i < ds->items.n; i++) {
				JVal *msg = json_get(ds->items.v[i], "message");
				printf("%ld # ", json_int(ds->items.v[i], "range.start.line") + 1);
				if (msg)
					fwrite(msg->s, 1, msg->n, stdout);
				putchar('\n');
			}
		}
		free(b);
	}
}

int main(int argc, char **argv) {
	if (argc == 2 && !strcmp(argv[1], "diags"))
		return diags();
	if (argc == 2 && !strcmp(argv[1], "split"))
		return split();
	if (argc < 4) {
		fprintf(stderr, "usage: lspsession basic|features FILE N [DIR ...] | split | diags\n");
		return 2;
	}
	const char *mode = argv[1];
	char path[PATH_MAX];
	if (!realpath(argv[2], path)) {
		perror(argv[2]);
		return 1;
	}
	int n = atoi(argv[3]);
	FILE *f = fopen(path, "rb");
	Buf src = {0};
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&src, chunk, k);
	fclose(f);
	if (!src.b)
		bufputs(&src, "");
	Buf uri = {0};
	bufputs(&uri, "file://");
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    strchr("-._~$&+,/:;=@", *p))
			bufputc(&uri, *p);
		else
			bufprintf(&uri, "%%%02X", *p);
	}
	Buf p = {0};
	bufprintf(&p, "{\"processId\":null,\"rootUri\":null,\"capabilities\":{\"textDocument\":{\"documentSymbol\":"
		      "{\"hierarchicalDocumentSymbolSupport\":%s}}},\"initializationOptions\":{%s\"ipath\":[",
		!strcmp(mode, "basic") && n == 1 ? "true" : !strcmp(mode, "basic") ? "false" : "true",
		getenv("LSPICON95") ? "\"icon95\":true," : "");
	for (int i = 4; i < argc; i++) {
		if (i > 4)
			bufputc(&p, ',');
		json_stringz(&p, argv[i]);
	}
	bufputs(&p, "]}}");
	request("initialize", p.b);
	notification("initialized", "{}");
	p.n = 0;
	bufputs(&p, "{\"textDocument\":{\"uri\":");
	json_stringz(&p, uri.b);
	bufputs(&p, ",\"languageId\":\"icon\",\"version\":1,\"text\":");
	json_string(&p, src.b, src.n);
	bufputs(&p, "}}");
	notification("textDocument/didOpen", p.b);
	Buf td = {0};
	bufputs(&td, "{\"textDocument\":{\"uri\":");
	json_stringz(&td, uri.b);
	bufputs(&td, "}");
	p.n = 0;
	bufprintf(&p, "%s}", td.b);
	request("textDocument/documentSymbol", p.b);
	request("textDocument/formatting", p.b);
	if (!strcmp(mode, "features")) {
		/* positions of every nth identifier, as UTF-16 positions of
		 * an ASCII line (non-ASCII lines are skipped) */
		Tokens ts = lex_tokenize(src.b, src.n);
		Lines lines = lines_new(src.b, src.n);
		int count = 0, npos = 0;
		const char *ce = getenv("LSPCOMPLETE"); /* completion at every nth position */
		int cevery = ce ? atoi(ce) : 1;
		for (size_t i = 0; i < ts.n; i++) {
			Token *t = &ts.v[i];
			if (t->kind != T_IDENT && t->kind != T_FAIL)
				continue;
			if (count++ % (n > 0 ? n : 1))
				continue;
			int line, col;
			lines_position(&lines, t->offset + t->len / 2, &line, &col);
			/* the line's text must be ASCII up to the column */
			bool ascii = true;
			for (size_t j = t->offset + t->len / 2 - (size_t)col; j < t->offset + t->len / 2; j++)
				if ((unsigned char)src.b[j] >= 0x80)
					ascii = false;
			if (!ascii)
				continue;
			Buf q = {0};
			bufprintf(&q, "%s,\"position\":{\"line\":%d,\"character\":%d}", td.b, line - 1, col);
			Buf r = {0};
			bufprintf(&r, "%s}", q.b);
			request("textDocument/hover", r.b);
			request("textDocument/definition", r.b);
			r.n = 0;
			bufprintf(&r, "%s,\"context\":{\"includeDeclaration\":true}}", q.b);
			request("textDocument/references", r.b);
			r.n = 0;
			bufprintf(&r, "%s,\"context\":{\"includeDeclaration\":false}}", q.b);
			request("textDocument/references", r.b);
			r.n = 0;
			bufprintf(&r, "%s}", q.b);
			request("textDocument/documentHighlight", r.b);
			request("textDocument/signatureHelp", r.b);
			if (cevery > 0 && npos % cevery == 0)
				request("textDocument/completion", r.b);
			npos++;
			request("textDocument/prepareRename", r.b);
			r.n = 0;
			bufprintf(&r, "%s,\"newName\":\"zzz\"}", q.b);
			request("textDocument/rename", r.b);
		}
		request("workspace/symbol", "{\"query\":\"\"}");
		request("workspace/symbol", "{\"query\":\"e\"}");
	}
	request("shutdown", "null");
	notification("exit", "null");
	(void)out;
	return 0;
}
