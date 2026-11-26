/*
 * Files (gcon's file.go and kbd.go): open files, pipes, directories and
 * the standard streams, read and written through buffers of our own on
 * file descriptors. Files are text unless opened with "u".
 */
#include "vm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "gfx.h"

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)

/* The buffers: File.r and File.w are unused as FILE*; we keep our own. */
typedef struct IOBuf {
	int fd;
	char *b;
	size_t pos, len, cap;
	FILE *popen; /* a pipe's stream, for pclose */
} IOBuf;

#define RB(f) ((IOBuf *)(f)->r)
#define WB(f) ((IOBuf *)(f)->w)

static IOBuf *newbuf(int fd, size_t cap) {
	IOBuf *b = xcalloc(1, sizeof *b);
	b->fd = fd;
	b->cap = cap;
	b->b = xmalloc(cap);
	return b;
}

static void wflush(IOBuf *w) {
	size_t off = 0;
	while (off < w->len) {
		ssize_t k = write(w->fd, w->b + off, w->len - off);
		if (k <= 0) {
			if (k < 0 && errno == EINTR)
				continue;
			break;
		}
		off += (size_t)k;
	}
	w->len = 0;
}

static void wwrite(IOBuf *w, const char *p, size_t n) {
	if (w->len + n > w->cap) {
		wflush(w);
		if (n > w->cap) {
			w->len = n;
			char *save = w->b;
			w->b = (char *)p;
			wflush(w);
			w->b = save;
			return;
		}
	}
	memcpy(w->b + w->len, p, n);
	w->len += n;
}

/* rfill makes at least k bytes available if it can, reporting how many
 * are. */
static size_t rfill(IOBuf *r, size_t k) {
	if (r->len - r->pos >= k)
		return r->len - r->pos;
	if (r->pos > 0) {
		memmove(r->b, r->b + r->pos, r->len - r->pos);
		r->len -= r->pos;
		r->pos = 0;
	}
	while (r->len < k) {
		ssize_t m = read(r->fd, r->b + r->len, r->cap - r->len);
		if (m < 0 && errno == EINTR)
			continue;
		if (m <= 0)
			break;
		r->len += (size_t)m;
	}
	return r->len - r->pos;
}

static int rgetc(IOBuf *r) {
	if (rfill(r, 1) == 0)
		return -1;
	return (uint8_t)r->b[r->pos++];
}

static File *newfile(const char *name, bool keyword) {
	File *f = gcnew(sizeof *f, T_FILE);
	f->name = textc(name);
	f->keyword = keyword;
	f->fd = -1;
	f->peeked = -1;
	return f;
}

static void addfile(File *f) {
	if (vm.nfiles == vm.capfiles) {
		vm.capfiles = vm.capfiles ? 2 * vm.capfiles : 8;
		vm.files = realloc(vm.files, (size_t)vm.capfiles * sizeof(File *));
	}
	vm.files[vm.nfiles++] = f;
}

void files_init(void) {
	vm.input = newfile("&input", true);
	vm.input->r = (void *)newbuf(0, 65536);
	vm.output = newfile("&output", true);
	vm.output->w = (void *)newbuf(1, 65536);
	vm.errfile = newfile("&errout", true);
	vm.errfile->w = (void *)newbuf(2, 4096);
	vm.errfile->errout = true;
}

void flush_output(void) { wflush(WB(vm.output)); }

/* files_flush_all flushes every open file at exit, as C's exit does,
 * and closes the pipes written to, so their commands see the end of
 * their input, waiting for them so that their output is complete when
 * gcon-vm ends (iconx does not wait; gcon waits when it relays output). */
void files_flush_all(void) {
	wflush(WB(vm.output));
	for (int i = 0; i < vm.nfiles; i++) {
		File *f = vm.files[i];
		if (!f->w || f->closed)
			continue;
		wflush(WB(f));
		if (f->pipe) {
			f->closed = true;
			pclose(WB(f)->popen);
		}
	}
	wflush(WB(vm.errfile));
}

File *mustfile(Value v) {
	if (VKIND(v) != KFile)
		runerrv(105, v);
	return v.w.p;
}

static File *filearg(Value *a, int n, int i, File *d) {
	if (ISNULL(ARG(i)))
		return d;
	return mustfile(a[i]);
}

static IOBuf *reader(File *f) {
	if (!f->r || f->closed)
		runerrv(212, ptrval(KFile, f));
	if (f == vm.input)
		wflush(WB(vm.output)); /* let a prompt appear first */
	return RB(f);
}

static IOBuf *writer(File *f) {
	if (!f->w || f->closed)
		runerrv(213, ptrval(KFile, f));
	if (f->errout)
		wflush(WB(vm.output));
	return WB(f);
}

static bool nextentry(File *f, int64_t n, String *out) {
	if (f->idir >= f->ndir)
		return false;
	char *e = f->dir[f->idir++];
	int64_t k = (int64_t)strlen(e);
	if (n >= 0 && k > n)
		k = n;
	*out = mkstr(e, k, f->unit);
	return true;
}

/* readline_ reads a line: it ends at "\n", "\r\n" or a lone "\r", except
 * in an untranslated file, where only "\n" ends it. */
bool readline_(File *f, String *out) {
	if (f->gfx && !f->closed)
		return gfx_read(f, -1, out);
	IOBuf *r = reader(f);
	if (f->isdir)
		return nextentry(f, -1, out);
	SB b = {0};
	bool any = false;
	for (;;) {
		int c = rgetc(r);
		if (c < 0) {
			if (!any)
				return false;
			break;
		}
		any = true;
		if (c == '\n')
			break;
		if (c == '\r' && f->unit == TextUnit) {
			if (rfill(r, 1) > 0 && r->b[r->pos] == '\n')
				r->pos++;
			break;
		}
		sbputc(&b, c);
	}
	*out = sbstring(&b, f->unit);
	return true;
}

static bool readchars(File *f, int64_t k, String *out) {
	if (f->gfx && !f->closed)
		return gfx_read(f, k, out);
	IOBuf *r = reader(f);
	if (f->isdir)
		return nextentry(f, k, out);
	SB b = {0};
	for (int64_t i = 0; i < k; i++) {
		if (f->unit == BytesUnit) {
			int c = rgetc(r);
			if (c < 0)
				break;
			sbputc(&b, c);
			continue;
		}
		size_t avail = rfill(r, 4);
		if (avail == 0)
			break;
		int w;
		utf8decode((const uint8_t *)r->b + r->pos, avail < 4 ? avail : 4, &w);
		sbadd(&b, r->b + r->pos, (size_t)w);
		r->pos += (size_t)w;
	}
	if (b.n == 0)
		return false;
	*out = sbstring(&b, f->unit);
	return true;
}

void emit(File *f, const char *b, size_t n, bool end) {
	if (f->gfx && !f->closed) { /* a window: drawn, at its text position */
		gfx_write(f, b, n);
		return;
	}
	IOBuf *w = writer(f);
	if (f->pipe)
		wflush(WB(vm.output));
	wwrite(w, b, n);
	if (end && f != vm.output)
		wflush(w);
}

/* writeargs implements write (nl) and writes. */
static Value writeargs(File *f, Value *a, int n, bool nl) {
	Value last = NULLV;
	Buf buf = {0};
	for (int k = 0; k < n; k++) {
		Value v = a[k];
		last = v;
		if (VKIND(v) == KFile) {
			if (nl && k > 0) {
				bufputc(&buf, '\n');
				emit(f, buf.b, buf.n, true);
			} else if (buf.n > 0)
				emit(f, buf.b, buf.n, false);
			buf.n = 0;
			f = v.w.p;
		} else if (VKIND(v) != KNull) {
			String s;
			if (!cvstr(v, &s)) {
				if (!f->gfx)
					writer(f);
				if (f->errout || f->gfx)
					emit(f, buf.b, buf.n, true);
				else {
					vm.partial = buf;
					vm.partialfile = f;
					vm.haspartial = true;
				}
				runerrv(109, v);
			}
			bufadd(&buf, s.s, (size_t)s.n);
			if (f->gfx && !f->closed) { /* a window: each argument drawn as it comes, as iconx's wputstr */
				emit(f, buf.b, buf.n, false);
				buf.n = 0;
			}
		}
	}
	if (nl)
		bufputc(&buf, '\n');
	emit(f, buf.b ? buf.b : "", buf.n, true);
	return last;
}

FN(f_write) { (void)b; *out = writeargs(vm.output, a, n, true); return true; }
FN(f_writes) { (void)b; *out = writeargs(vm.output, a, n, false); return true; }

FN(f_stop) {
	(void)b;
	(void)out;
	writeargs(vm.errfile, a, n, true);
	vm_exit(1);
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool openfile(String name, String mode, File **out) {
	File *f = newfile("", false);
	f->name = name;
	f->unit = TextUnit;
	char *cname = cstr(name);
	bool has[128] = {0};
	for (int64_t i = 0; i < mode.n; i++) {
		char c = mode.s[i];
		if (!strchr("rwabctupRWABCTUP", c) || c == 0)
			runerrv(209, strval(mode));
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		has[(int)c] = true;
	}
	if (has['u'])
		f->unit = BytesUnit;
	if (has['p']) {
		wflush(WB(vm.output));
		wflush(WB(vm.errfile));
		FILE *p = popen(cname, has['w'] ? "w" : "r");
		if (!p)
			return false;
		IOBuf *b = newbuf(fileno(p), 65536);
		b->popen = p;
		f->pipe = true;
		if (has['w']) {
			f->w = (void *)b;
			addfile(f);
		} else
			f->r = (void *)b;
		*out = f;
		return true;
	}
	bool rd = has['r'] || has['b'];
	bool wr = has['w'] || has['b'] || has['a'] || has['c'];
	if (!rd && !wr)
		rd = true;
	int flag;
	if (has['c'])
		flag = O_CREAT | O_TRUNC;
	else if (has['a'])
		flag = O_CREAT | O_APPEND;
	else if (rd)
		flag = 0;
	else
		flag = O_CREAT | O_TRUNC;
	flag |= rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
	struct stat st;
	bool isdir = stat(cname, &st) == 0 && S_ISDIR(st.st_mode);
	if (isdir) {
		if (wr)
			return false;
		DIR *d = opendir(cname);
		if (!d)
			return false;
		int cap = 16, k = 0;
		char **names = xmalloc((size_t)cap * sizeof(char *));
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			if (k == cap)
				names = realloc(names, (size_t)(cap *= 2) * sizeof(char *));
			names[k++] = strdup(e->d_name);
		}
		closedir(d);
		qsort(names, (size_t)k, sizeof(char *), cmpstr);
		f->dir = xmalloc((size_t)(k + 2) * sizeof(char *));
		f->dir[0] = ".";
		f->dir[1] = "..";
		memcpy(f->dir + 2, names, (size_t)k * sizeof(char *));
		f->ndir = k + 2;
		f->isdir = true;
		f->r = (void *)newbuf(-1, 16);
		*out = f;
		return true;
	}
	int fd = open(cname, flag, 0666);
	if (fd < 0)
		return false;
	f->fd = fd;
	if (rd)
		f->r = (void *)newbuf(fd, 65536);
	if (wr) {
		f->w = (void *)newbuf(fd, 65536);
		addfile(f);
	}
	*out = f;
	return true;
}


FN(f_open) {
	(void)b;
	String mode = textc("r");
	if (!ISNULL(ARG(1)))
		mode = muststr(a[1]);
	if (memchr(mode.s, 'g', (size_t)mode.n) || memchr(mode.s, 'G', (size_t)mode.n) ||
	    memchr(mode.s, 'x', (size_t)mode.n) || memchr(mode.s, 'X', (size_t)mode.n)) { /* x: as iconx, a window too */
		/* a window: the other arguments are its attributes */
		File *w = gfx_open(muststr(ARG(0)), a + 2, n > 2 ? n - 2 : 0);
		if (!w)
			return false;
		*out = ptrval(KFile, w);
		return true;
	}
	File *f;
	if (!openfile(muststr(ARG(0)), mode, &f))
		return false;
	*out = ptrval(KFile, f);
	return true;
}

FN(f_close) {
	(void)b;
	File *f = mustfile(ARG(0));
	*out = ptrval(KFile, f);
	if (f->closed)
		return true;
	if (f->gfx) {
		gfx_close(f);
		f->closed = true;
		return true;
	}
	if (f->w)
		wflush(WB(f));
	if (f->keyword)
		return true;
	if (f->pipe)
		wflush(WB(vm.output));
	f->closed = true;
	if (f->pipe) {
		IOBuf *p = f->w ? WB(f) : RB(f);
		int st = pclose(p->popen);
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
		*out = intval(code & 0xff);
		return true;
	}
	if (f->fd >= 0)
		close(f->fd);
	return true;
}

FN(f_flush) {
	(void)b;
	File *f = mustfile(ARG(0));
	if (f->w)
		wflush(WB(f));
	*out = ptrval(KFile, f);
	return true;
}

FN(f_read) {
	(void)b;
	String s;
	if (!readline_(filearg(a, n, 0, vm.input), &s))
		return false;
	*out = strval(s);
	return true;
}

FN(f_reads) {
	(void)b;
	File *f = filearg(a, n, 0, vm.input);
	int64_t k = 1;
	if (!ISNULL(ARG(1)))
		k = mustint(a[1]);
	if (k <= 0)
		runerrv(205, intval(k));
	String s;
	if (!readchars(f, k, &s))
		return false;
	*out = strval(s);
	return true;
}

FN(f_seek) {
	(void)b;
	File *f = mustfile(ARG(0));
	int64_t i = mustint(ARG(1));
	if (f->fd < 0)
		return false;
	if (f->w)
		wflush(WB(f));
	off_t r = i > 0 ? lseek(f->fd, i - 1, SEEK_SET) : lseek(f->fd, i, SEEK_END);
	if (r < 0)
		return false;
	if (f->r)
		RB(f)->pos = RB(f)->len = 0;
	*out = ptrval(KFile, f);
	return true;
}

FN(f_where) {
	(void)b;
	File *f = mustfile(ARG(0));
	if (f->fd < 0)
		return false;
	if (f->w)
		wflush(WB(f));
	off_t off = lseek(f->fd, 0, SEEK_CUR);
	if (off < 0)
		return false;
	if (f->r)
		off -= (off_t)(RB(f)->len - RB(f)->pos);
	*out = intval(off + 1);
	return true;
}

FN(f_remove) {
	(void)b;
	*out = NULLV;
	return remove(cstr(muststr(ARG(0)))) == 0;
}

FN(f_rename) {
	(void)b;
	*out = NULLV;
	return rename(cstr(muststr(ARG(0))), cstr(muststr(ARG(1)))) == 0;
}

FN(f_getenv) {
	(void)b;
	char *v = getenv(cstr(muststr(ARG(0))));
	if (!v)
		return false;
	*out = strval(textc(v));
	return true;
}

FN(f_system) {
	(void)b;
	wflush(WB(vm.output));
	wflush(WB(vm.errfile));
	int st = system(cstr(muststr(ARG(0))));
	int code;
	if (st == -1)
		code = 127 << 8;
	else if (WIFEXITED(st))
		code = WEXITSTATUS(st) << 8;
	else
		code = -256;
	*out = intval(code);
	return true;
}

FN(k_input) { (void)b; (void)a; (void)n; *out = ptrval(KFile, vm.input); return true; }
FN(k_output) { (void)b; (void)a; (void)n; *out = ptrval(KFile, vm.output); return true; }
FN(k_errout) { (void)b; (void)a; (void)n; *out = ptrval(KFile, vm.errfile); return true; }

/* ---- The keyboard functions, as iconx's on Unix ---- */

static bool rawmode(struct termios *old, bool echo, bool block) {
	if (tcgetattr(0, old) != 0)
		return false;
	struct termios t = *old;
	t.c_lflag &= ~(tcflag_t)ICANON;
	if (echo)
		t.c_lflag |= ECHO;
	else
		t.c_lflag &= ~(tcflag_t)ECHO;
	t.c_cc[VMIN] = block ? 1 : 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
	return true;
}

static bool getkey(bool echo, Value *out) {
	IOBuf *r = reader(vm.input);
	struct termios old;
	bool tty = rawmode(&old, echo, true);
	int c = rgetc(r);
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	if (c < 0)
		return false;
	char ch = (char)c;
	*out = strval(strcopy(&ch, 1, c < 0x80 ? TextUnit : BytesUnit));
	return true;
}

FN(f_getch) { (void)b; (void)a; (void)n; return getkey(false, out); }
FN(f_getche) { (void)b; (void)a; (void)n; return getkey(true, out); }

FN(f_kbhit) {
	(void)b;
	(void)a;
	(void)n;
	IOBuf *r = reader(vm.input);
	*out = NULLV;
	if (r->len > r->pos)
		return true;
	struct termios old;
	bool tty = rawmode(&old, true, false);
	size_t k = rfill(r, 1);
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	return k > 0 || !tty;
}

static Builtin ops[] = {
	{.name = "&input", .key = "&input", .fn = k_input},
	{.name = "&output", .key = "&output", .fn = k_output},
	{.name = "&errout", .key = "&errout", .fn = k_errout},
};

static Builtin fns[] = {
	{.name = "write", .fn = f_write}, {.name = "writes", .fn = f_writes},
	{.name = "stop", .fn = f_stop}, {.name = "open", .fn = f_open},
	{.name = "close", .fn = f_close}, {.name = "flush", .fn = f_flush},
	{.name = "read", .fn = f_read}, {.name = "reads", .fn = f_reads},
	{.name = "seek", .fn = f_seek}, {.name = "where", .fn = f_where},
	{.name = "remove", .fn = f_remove}, {.name = "rename", .fn = f_rename},
	{.name = "getenv", .fn = f_getenv}, {.name = "system", .fn = f_system},
	{.name = "getch", .fn = f_getch}, {.name = "getche", .fn = f_getche},
	{.name = "kbhit", .fn = f_kbhit},
};

void file_fns_init(void) {
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}

/* ---- Files for C functions (cfunc.c) ---- */

/* file_lend returns the descriptor beneath f for a C function, with how
 * it is open in iconx's terms (1 read, 2 write, 020 pipe), after
 * writing out what f has buffered; -1 if f is closed or a directory.
 * What f has read ahead stays in f's buffer, unseen by C. */
int file_lend(File *f, int *stat) {
	*stat = 0;
	if (f->closed || f->isdir)
		return -1;
	if (f->r)
		*stat |= 1;
	if (f->w) {
		*stat |= 2;
		wflush(WB(f));
	}
	if (f->pipe)
		*stat |= 020;
	IOBuf *b = f->r ? RB(f) : WB(f);
	return b ? b->fd : -1;
}

/* file_adopt makes a file of a stream a C function opened (icall.h's
 * RetFile): a pipe keeps its stream, for pclose; any other file keeps a
 * duplicate of its descriptor, and the stream is closed. */
File *file_adopt(FILE *fp, int stat, String name) {
	File *f = newfile("", false);
	f->name = name;
	f->unit = TextUnit;
	int fd;
	fflush(fp);
	if (stat & 020) {
		fd = fileno(fp);
		f->pipe = true;
	} else {
		fd = dup(fileno(fp));
		fclose(fp);
		f->fd = fd;
	}
	if ((stat & 3) == 0)
		stat |= 1;
	if (stat & 1)
		f->r = (void *)newbuf(fd, 65536);
	if (stat & 2) {
		f->w = (void *)newbuf(fd, 65536);
		addfile(f);
	}
	if (f->pipe)
		((IOBuf *)(f->w ? f->w : f->r))->popen = fp;
	return f;
}

/* file_window makes the file value of a window (graphics.c): neither
 * read nor written through file.c's buffers. */
File *file_window(String name, void *binding) {
	File *f = newfile("", false);
	f->name = name;
	f->unit = TextUnit;
	f->gfx = binding;
	return f;
}
