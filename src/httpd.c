/*
 * A small HTTP/1.1 server (httpd.h).
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "httpd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
	MaxHead = 64 << 10, /* the request line and headers */
	MaxBody = 1 << 20,
};

/* splithost splits "host:port" ("[::1]:80" too) into its parts. */
static bool splithost(const char *addr, char **host, char **port) {
	const char *colon = strrchr(addr, ':');
	if (!colon)
		return false;
	const char *h = addr;
	size_t n = (size_t)(colon - addr);
	if (n >= 2 && h[0] == '[' && h[n - 1] == ']')
		h++, n -= 2;
	*host = xstrndup(h, n);
	*port = xstrndup(colon + 1, strlen(colon + 1));
	return true;
}

bool http_isloopback(const char *addr) {
	char *host, *port;
	if (!splithost(addr, &host, &port))
		return false;
	return !strcmp(host, "localhost") || !strncmp(host, "127.", 4) || !strcmp(host, "::1");
}

int http_listen(const char *addr, char **bound) {
	char *host, *port;
	if (!splithost(addr, &host, &port)) {
		fprintf(stderr, "gcon doc: %s: the address is not host:port\n", addr);
		return -1;
	}
	struct addrinfo hints = {0}, *ai;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	int e = getaddrinfo(*host ? host : NULL, port, &hints, &ai);
	if (e) {
		fprintf(stderr, "gcon doc: %s: %s\n", addr, gai_strerror(e));
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *a = ai; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 64) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd < 0) {
		fprintf(stderr, "gcon doc: listen %s: %s\n", addr, strerror(errno));
		return -1;
	}
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	char hb[NI_MAXHOST], pb[NI_MAXSERV];
	Buf b = {0};
	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 &&
	    getnameinfo((struct sockaddr *)&ss, sl, hb, sizeof hb, pb, sizeof pb, NI_NUMERICHOST | NI_NUMERICSERV) == 0)
		bufprintf(&b, strchr(hb, ':') ? "[%s]:%s" : "%s:%s", hb, pb);
	else
		bufputs(&b, addr);
	*bound = b.b;
	return fd;
}

static int hexval(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static char *pctdecode(const char *s, size_t n) {
	Buf b = {0};
	for (size_t i = 0; i < n; i++) {
		int h, l;
		if (s[i] == '%' && i + 2 < n && (h =hexval(s[i + 1])) >= 0 && (l = hexval(s[i + 2])) >= 0) {
			bufputc(&b, (char)(h << 4 | l));
			i += 2;
		} else
			bufputc(&b, s[i]);
	}
	return b.b ? b.b : xstrndup("", 0);
}

/* readreq reads one request; false if there is none to answer. */
static bool readreq(HttpReq *r) {
	Buf b = {0};
	char chunk[8192];
	char *end;
	for (;;) {
		ssize_t k = read(r->fd, chunk, sizeof chunk);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return false;
		bufadd(&b, chunk, (size_t)k);
		if ((end = strstr(b.b, "\r\n\r\n")))
			break;
		if (b.n > MaxHead)
			return false;
	}
	size_t headn = (size_t)(end - b.b) + 4;
	/* the request line */
	char *line = b.b, *nl = strstr(line, "\r\n");
	*nl = 0;
	char *sp1 = strchr(line, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
	if (!sp1 || !sp2)
		return false;
	r->method = xstrndup(line, (size_t)(sp1 - line));
	char *target = sp1 + 1;
	size_t tn = (size_t)(sp2 - target);
	char *q = memchr(target, '?', tn);
	r->path = pctdecode(target, q ? (size_t)(q - target) : tn);
	r->query = q ? xstrndup(q + 1, (size_t)(sp2 - q - 1)) : "";
	/* the headers */
	for (char *h = nl + 2; h < end;) {
		char *e = strstr(h, "\r\n");
		char *hd = xstrndup(h, (size_t)(e - h));
		for (char *c = hd; *c && *c != ':'; c++)
			*c = (char)tolower((unsigned char)*c);
		VPUSH(r->hdrs, hd);
		h = e + 2;
	}
	/* the body */
	const char *cl = http_header(r, "content-length");
	size_t want = cl ? strtoul(cl, NULL, 10) : 0;
	if (want > MaxBody)
		return false;
	Buf body = {0};
	bufadd(&body, b.b + headn, b.n - headn);
	while (body.n < want) {
		ssize_t k = read(r->fd, chunk, sizeof chunk);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return false;
		bufadd(&body, chunk, (size_t)k);
	}
	if (!body.b)
		bufputs(&body, "");
	r->body = body.b;
	r->nbody = want < body.n ? want : body.n;
	return true;
}

const char *http_header(const HttpReq *r, const char *name) {
	size_t n = strlen(name);
	for (size_t i = 0; i < r->hdrs.n; i++) {
		const char *h = r->hdrs.v[i];
		if (!strncmp(h, name, n) && h[n] == ':') {
			h += n + 1;
			while (*h == ' ' || *h == '\t')
				h++;
			return h;
		}
	}
	return NULL;
}

static const char *reason(int status) {
	switch (status) {
	case 200: return "OK";
	case 302: return "Found";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	default: return "Error";
	}
}

static void writeall(int fd, const char *s, size_t n) {
	while (n) {
		ssize_t k = write(fd, s, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return;
		s += k;
		n -= (size_t)k;
	}
}

void http_reply(HttpReq *r, int status, const char *ctype, const char *extra, const char *body, size_t n) {
	Buf h = {0};
	bufprintf(&h,
		"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n",
		status, reason(status), ctype, n, extra ? extra : "");
	writeall(r->fd, h.b, h.n);
	if (strcmp(r->method, "HEAD"))
		writeall(r->fd, body, n);
}

_Noreturn void http_serve(int fd, void (*h)(HttpReq *r, void *arg), void *arg) {
	signal(SIGCHLD, SIG_IGN); /* children reaped by the system */
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		int c = accept(fd, NULL, NULL);
		if (c < 0) {
			if (errno != EINTR && errno != ECONNABORTED)
				perror("gcon doc: accept");
			continue;
		}
		pid_t pid = fork();
		if (pid == 0) {
			close(fd);
			signal(SIGCHLD, SIG_DFL); /* the handler waits for its own */
			struct timeval tv = {10, 0};
			setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
			HttpReq r = {0};
			r.fd = c;
			if (readreq(&r))
				h(&r, arg);
			shutdown(c, SHUT_WR);
			close(c);
			_exit(0);
		}
		close(c);
	}
}
