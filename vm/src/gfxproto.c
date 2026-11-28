/*
 * The client side of the gcon graphics protocol (doc/internals/protocol.md):
 * starting the display server, buffering requests,
 * reading replies.
 *
 * The server is started on the first graphics request, as gcon's design
 * has it: a child on a socketpair, found by $GCONGRAPHICS, else as
 * gcon-graphics beside this executable, else on $PATH. Requests that
 * need no reply are buffered and sent when a reply is needed, when the
 * program waits, flushes or exits.
 */
#include "gfx.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

enum { Version = 3 };

static int sock = -1;
static pid_t server;
static uint8_t *ob;           /* requests not yet sent */
static size_t olen, ocap, ostart;
static uint8_t *rb;           /* the last reply's body */
static size_t rlen, rcap, rpos;

_Noreturn static void lost(const char *why) {
	fprintf(stderr, "gcon-vm: graphics: %s\n", why);
	files_flush_all();
	exit(1);
}

/* selfexe gives gcon-vm's own executable's path (PATH_MAX bytes). */
bool selfexe(char *self) {
#ifdef __APPLE__
	uint32_t n = PATH_MAX;
	char raw[PATH_MAX];
	return _NSGetExecutablePath(raw, &n) == 0 && realpath(raw, self);
#else
	ssize_t k = readlink("/proc/self/exe", self, PATH_MAX - 1);
	if (k <= 0)
		return false;
	self[k] = 0;
	return true;
#endif
}

/* serverpath finds the display server's executable. */
static char *serverpath(void) {
	const char *e = getenv("GCONGRAPHICS");
	if (e && *e)
		return strdup(e);
	char self[PATH_MAX];
	if (selfexe(self)) {
		char *slash = strrchr(self, '/');
		if (slash) {
			strcpy(slash + 1, "gcon-graphics");
			if (access(self, X_OK) == 0)
				return strdup(self);
		}
	}
	return strdup("gcon-graphics");
}

static void writeall(const uint8_t *p, size_t n) {
	while (n > 0) {
		ssize_t k = write(sock, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			lost("the display server has gone");
		p += k;
		n -= (size_t)k;
	}
}

static void readall(void *p, size_t n) {
	uint8_t *b = p;
	while (n > 0) {
		ssize_t k = read(sock, b, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			lost("the display server has gone");
		b += k;
		n -= (size_t)k;
	}
}

/* ---- Requests ---- */

static void grow(size_t k) {
	if (olen + k > ocap) {
		ocap = (olen + k) * 2 + 4096;
		ob = realloc(ob, ocap);
		if (!ob)
			lost("out of memory");
	}
}

void gp_begin(int op) {
	grow(5);
	ostart = olen;
	olen += 4; /* the length, set by gp_end */
	ob[olen++] = (uint8_t)op;
}

void gp_u8(int v) {
	grow(1);
	ob[olen++] = (uint8_t)v;
}

void gp_u32(uint32_t v) {
	grow(4);
	ob[olen++] = (uint8_t)v;
	ob[olen++] = (uint8_t)(v >> 8);
	ob[olen++] = (uint8_t)(v >> 16);
	ob[olen++] = (uint8_t)(v >> 24);
}

void gp_i32(int32_t v) { gp_u32((uint32_t)v); }

void gp_f64(double d) {
	uint64_t v;
	memcpy(&v, &d, 8);
	gp_u32((uint32_t)v);
	gp_u32((uint32_t)(v >> 32));
}

void gp_bytes(const void *p, size_t n) {
	grow(n);
	memcpy(ob + olen, p, n);
	olen += n;
}

void gp_str(const char *s, size_t n) {
	gp_u32((uint32_t)n);
	gp_bytes(s, n);
}

void gp_end(void) {
	uint32_t n = (uint32_t)(olen - ostart - 4);
	ob[ostart] = (uint8_t)n;
	ob[ostart + 1] = (uint8_t)(n >> 8);
	ob[ostart + 2] = (uint8_t)(n >> 16);
	ob[ostart + 3] = (uint8_t)(n >> 24);
	if (olen > (1u << 20))
		gp_flush();
}

void gp_flush(void) {
	if (olen > 0 && sock >= 0)
		writeall(ob, olen);
	olen = 0;
}

/* ---- Replies ---- */

void gp_reply(int op) {
	gp_flush();
	uint8_t h[4];
	readall(h, 4);
	size_t n = h[0] | h[1] << 8 | h[2] << 16 | (size_t)h[3] << 24;
	if (n < 1)
		lost("protocol error (empty reply)");
	if (n > rcap) {
		rcap = n;
		rb = realloc(rb, rcap);
		if (!rb)
			lost("out of memory");
	}
	readall(rb, n);
	if (rb[0] != op)
		lost("protocol error (reply out of order)");
	rlen = n;
	rpos = 1;
}

uint32_t gr_u32(void) {
	if (rlen - rpos < 4)
		lost("protocol error (short reply)");
	uint8_t *p = rb + rpos;
	rpos += 4;
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

int32_t gr_i32(void) { return (int32_t)gr_u32(); }

int64_t gr_i64(void) {
	uint64_t lo = gr_u32();
	uint64_t hi = gr_u32();
	return (int64_t)(lo | hi << 32);
}

/* ---- Connecting ---- */

bool gp_connect(void) {
	if (sock >= 0)
		return true;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return false;
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	signal(SIGPIPE, SIG_IGN); /* a lost server is reported, not a signal */
	char *path = serverpath();
	files_flush_all();
	server = fork();
	if (server < 0) {
		close(sv[0]);
		close(sv[1]);
		free(path);
		return false;
	}
	if (server == 0) {
		close(sv[0]);
		char fd[16];
		snprintf(fd, sizeof fd, "%d", sv[1]);
		char *argv[] = {path, "-fd", fd, NULL};
		execvp(path, argv);
		fprintf(stderr, "gcon-vm: cannot run the display server %s: %s\n", path, strerror(errno));
		_exit(127);
	}
	free(path);
	close(sv[1]);
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	sock = sv[0];
	atexit(gp_close); /* the server closes the windows and exits */
	gp_begin(P_HELLO);
	gp_u32(Version);
	gp_end();
	gp_reply(P_HELLO);
	gr_u32();
	if (!gr_u32())
		lost("the display server speaks another version of the protocol");
	return true;
}

/* gp_close ends the connection (at exit): the server closes the
 * windows and exits. */
void gp_close(void) {
	if (sock < 0)
		return;
	gp_flush();
	close(sock);
	sock = -1;
	waitpid(server, NULL, 0);
}

void gr_bytes(void *p, size_t n) {
	if (rlen - rpos < n)
		lost("protocol error (short reply)");
	memcpy(p, rb + rpos, n);
	rpos += n;
}
