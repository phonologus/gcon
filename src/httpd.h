/*
 * A small HTTP/1.1 server (httpd.c), for gcon doc's local server: one
 * request per connection, each connection handled in a child process.
 * Not a general web server: requests are small, replies are whole.
 */
#ifndef GCON_HTTPD_H
#define GCON_HTTPD_H

#include "util.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct HttpReq {
	int fd;
	char *method;
	char *path;  /* percent-decoded, without the query */
	char *query; /* raw, or "" */
	VEC(char *) hdrs; /* "name: value", the name lower case */
	char *body;
	size_t nbody;
} HttpReq;

/* http_listen listens on addr ("host:port", port 0 for any free one)
 * and returns the socket, or -1 with a message on standard error;
 * *bound is the address it has ("127.0.0.1:54321"). */
int http_listen(const char *addr, char **bound);

/* http_serve accepts connections on fd for ever, calling h for each
 * request in a child process of its own. */
_Noreturn void http_serve(int fd, void (*h)(HttpReq *r, void *arg), void *arg);

/* http_header is the value of the request's header name (lower case),
 * or NULL. */
const char *http_header(const HttpReq *r, const char *name);

/* http_reply sends the whole response; extra is more header lines, each
 * ending in "\r\n", or NULL. */
void http_reply(HttpReq *r, int status, const char *ctype, const char *extra, const char *body, size_t n);

/* http_isloopback reports whether addr's host is a loopback address. */
bool http_isloopback(const char *addr);

#endif
