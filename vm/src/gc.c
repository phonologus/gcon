/*
 * The garbage collector (docs/design.md, "The collector"): precise mark
 * and sweep, run only at the interpreter's safe point (the top of its
 * loop, between instructions), when no C code holds a heap pointer
 * that the roots do not. So builtins allocate freely, and the roots are
 * exactly the VM's: globals, constants, frames and co-expressions, a
 * few fields of the VM, and statics registered with gcroot.
 *
 * The heap is 1 MB chunks, each of blocks of one size class, and larger
 * objects in chunks of their own. Every block has a type, so a pointer
 * into any part of an object (a substring, a variable that is a local
 * of a frame) finds the object and how to trace it.
 */
#include "vm.h"
#include "gfx.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK ((size_t)1 << 20)
#define CHUNKSHIFT 20
/* A small-object chunk begins with a pointer to its Chunk, so that the
 * allocator finds a block's chunk by masking its address. */
#define HDR 64

static const uint32_t classes[] = {
	16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
	640, 768, 1024, 1280, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768,
};
enum { NClasses = sizeof classes / sizeof classes[0], MaxSmall = 32768 };

typedef struct Chunk {
	char *base;
	char *data;    /* the first block: after the header in a small-object chunk */
	size_t size;   /* CHUNK, or a large object's size */
	int cls;       /* size class, or -1 for a large object */
	uint32_t bsize, nblocks, used;
	uint64_t *mark, *alloc;
	uint8_t *type;
} Chunk;

static Chunk **chunks;
static int nchunks, capchunks;

/* The chunk map: from an address's megabyte to its chunk. */
static Chunk **map;
static uintptr_t *mapkey;
static size_t mapsize, mapcount;

static void *freelist[NClasses];
static Chunk *current[NClasses];
static uint8_t classof[MaxSmall / 16 + 1];

static size_t allocated, threshold = 16 * CHUNK, live;
bool gc_pending;
int64_t stress;
static int64_t stresscount;
static bool debug;
static int64_t ncollections;

static void ***roots;
static int nroots, caproots;

void gcroot(void **p) {
	if (nroots == caproots)
		roots = realloc(roots, (size_t)(caproots = caproots ? 2 * caproots : 16) * sizeof *roots);
	roots[nroots++] = p;
}

static size_t hashmb(uintptr_t k) { return (size_t)((k * 0x9E3779B97F4A7C15ULL) >> 20); }

static void mapput(uintptr_t k, Chunk *c);

static void mapgrow(void) {
	size_t old = mapsize;
	Chunk **om = map;
	uintptr_t *ok = mapkey;
	mapsize = mapsize ? 2 * mapsize : 1024;
	map = xcalloc(mapsize, sizeof *map);
	mapkey = xcalloc(mapsize, sizeof *mapkey);
	mapcount = 0;
	for (size_t i = 0; i < old; i++)
		if (om[i])
			mapput(ok[i], om[i]);
	free(om);
	free(ok);
}

static void mapput(uintptr_t k, Chunk *c) {
	if ((mapcount + 1) * 2 > mapsize)
		mapgrow();
	size_t i = hashmb(k) & (mapsize - 1);
	while (map[i] && mapkey[i] != k)
		i = (i + 1) & (mapsize - 1);
	if (!map[i])
		mapcount++;
	map[i] = c;
	mapkey[i] = k;
}

static Chunk *lookup(const void *p) {
	if (!mapsize)
		return NULL;
	uintptr_t k = (uintptr_t)p >> CHUNKSHIFT;
	size_t i = hashmb(k) & (mapsize - 1);
	while (map[i]) {
		if (mapkey[i] == k)
			return map[i];
		i = (i + 1) & (mapsize - 1);
	}
	return NULL;
}

static void addchunk(Chunk *c) {
	if (nchunks == capchunks)
		chunks = realloc(chunks, (size_t)(capchunks = capchunks ? 2 * capchunks : 64) * sizeof *chunks);
	chunks[nchunks++] = c;
	for (size_t off = 0; off < c->size; off += CHUNK)
		mapput(((uintptr_t)c->base + off) >> CHUNKSHIFT, c);
}

static Chunk *newchunk(size_t size, int cls, uint32_t bsize) {
	void *p;
	if (posix_memalign(&p, CHUNK, size) != 0) {
		fputs("gcon-vm: out of memory\n", stderr);
		exit(2);
	}
	Chunk *c = xcalloc(1, sizeof *c);
	c->base = p;
	c->data = cls < 0 ? c->base : c->base + HDR;
	if (cls >= 0)
		*(Chunk **)c->base = c;
	c->size = size;
	c->cls = cls;
	c->bsize = bsize;
	c->nblocks = cls < 0 ? 1 : (uint32_t)((size - HDR) / bsize);
	size_t words = (c->nblocks + 63) / 64;
	c->mark = xcalloc(words, 8);
	c->alloc = xcalloc(words, 8);
	c->type = xcalloc(c->nblocks, 1);
	addchunk(c);
	return c;
}

#define BIT(a, i) ((a)[(i) >> 6] >> ((i) & 63) & 1)
#define SET(a, i) ((a)[(i) >> 6] |= (uint64_t)1 << ((i) & 63))
#define CLR(a, i) ((a)[(i) >> 6] &= ~((uint64_t)1 << ((i) & 63)))

static void *alloc(size_t n, int type, bool zero);

void *gcnew(size_t n, int type) { return alloc(n, type, true); }
void *gcraw(size_t n) { return alloc(n, T_RAW, false); }
void *gcalloc(size_t n, int type, bool zero) { return alloc(n, type, zero); }

/* Bytes allocated since the program started (&allocated): raw (strings,
 * digits) and other blocks. */
static uint64_t total_raw, total_blocks;

void gc_allocated(int64_t out[4]) {
	out[0] = (int64_t)(total_raw + total_blocks);
	out[1] = 0; /* no static region */
	out[2] = (int64_t)total_raw;
	out[3] = (int64_t)total_blocks;
}

/* gc_unallocated sets the counts back to a snapshot gc_allocated took:
 * &allocated's own allocations are not counted, as iconx's keyword
 * allocates nothing. */
void gc_unallocated(const int64_t snap[4]) {
	total_raw = (uint64_t)snap[2];
	total_blocks = (uint64_t)snap[3];
}

static void *alloc(size_t n, int type, bool zero) {
	if (n == 0)
		n = 1;
	if (type == T_RAW)
		total_raw += n;
	else
		total_blocks += n;
	if (n > MaxSmall) {
		size_t size = (n + CHUNK - 1) & ~(CHUNK - 1);
		Chunk *c = newchunk(size, -1, 0); /* one block of size bytes */
		c->used = 1;
		SET(c->alloc, 0);
		c->type[0] = (uint8_t)type;
		memset(c->base, 0, n);
		allocated += size;
		if (allocated > threshold)
			gc_pending = true;
		return c->base;
	}
	int cls = classof[(n + 15) / 16];
	char *p = freelist[cls];
	Chunk *c;
	if (p) {
		freelist[cls] = *(void **)p;
		c = *(Chunk **)((uintptr_t)p & ~(uintptr_t)(CHUNK - 1));
	} else {
		c = current[cls];
		if (!c || c->used == c->nblocks)
			c = current[cls] = newchunk(CHUNK, cls, classes[cls]);
		p = c->data + (size_t)c->used++ * c->bsize;
	}
	uint32_t i = (uint32_t)((size_t)(p - c->data) / c->bsize);
	SET(c->alloc, i);
	c->type[i] = (uint8_t)type;
	if (zero)
		memset(p, 0, c->bsize);
	allocated += c->bsize;
	if (allocated > threshold || (stress && ++stresscount >= stress))
		gc_pending = true;
	return p;
}

/* ---- Marking ---- */

typedef struct Work {
	char *p;
	uint8_t type;
	size_t size;
} Work;

static Work *stack;
static size_t nstack, capstack;

/* markptr marks the object p points into, if p is in the heap,
 * reporting whether it is. */
bool markptr(const void *p) {
	if (!p)
		return false;
	Chunk *c = lookup(p);
	if (!c)
		return false;
	if ((const char *)p < c->data)
		return false;
	size_t off = (size_t)((const char *)p - c->data), i, size;
	if (c->cls < 0) {
		if (off >= c->size)
			return false;
		i = 0, size = c->size;
	} else {
		i = off / c->bsize;
		if (i >= c->used)
			return false;
		size = c->bsize;
	}
	if (!BIT(c->alloc, i))
		return true; /* a stale pointer to a free block: nothing to keep */
	if (BIT(c->mark, i))
		return true;
	SET(c->mark, i);
	if (c->type[i] == T_RAW)
		return true;
	if (nstack == capstack)
		stack = realloc(stack, (capstack = capstack ? 2 * capstack : 1024) * sizeof *stack);
	stack[nstack++] = (Work){c->data + i * (c->cls < 0 ? 0 : c->bsize), c->type[i], size};
	return true;
}

static void markomap(OMap *m) {
	markptr(m->keys);
	markptr(m->vals);
	markptr(m->index);
}

static void markcset(Cset *c) {
	if (!markptr(c)) { /* a constant's: not in the heap, but its cache is */
		markptr(c->str);
	}
}

void markvalue(Value v) {
	switch (VKIND(v)) {
	case KString:
		markptr(v.w.s);
		break;
	case KCset:
		markcset(v.w.p);
		break;
	case KBig: case KList: case KTable: case KSet: case KRecord: case KCoexp:
	case KFile: case KRef: case KListElem: case KField: case KSubstr: case KVar:
	case KFrame: case KGen: case KScanEnv: case KExternal:
		markptr(v.w.p);
		break;
	}
}

static void markvalues(const Value *v, size_t n) {
	for (size_t i = 0; i < n; i++)
		markvalue(v[i]);
}

static void trace(Work w) {
	switch (w.type) {
	case T_VALUES:
		markvalues((Value *)w.p, w.size / sizeof(Value));
		break;
	case T_LIST:
		markptr(((List *)w.p)->e);
		break;
	case T_RECORD: {
		Record *r = (Record *)w.p;
		markvalues(r->f, (size_t)r->t->nfields);
		break;
	}
	case T_TABLE: {
		Table *t = (Table *)w.p;
		markomap(&t->m);
		markvalue(t->dflt);
		break;
	}
	case T_SET:
		markomap(&((Set *)w.p)->m);
		break;
	case T_SUBSTR:
		markvalue(((Substr *)w.p)->v);
		break;
	case T_VAR: {
		Var *v = (Var *)w.p;
		if (v->kind == VTableElem) {
			markptr(v->t);
			markvalue(v->k);
		}
		break;
	}
	case T_SCANENV:
		markptr(((ScanEnv *)w.p)->subject.s);
		break;
	case T_GEN: {
		Gen *g = (Gen *)w.p;
		if (g->mark)
			g->mark(g);
		break;
	}
	case T_FRAME: {
		Frame *f = (Frame *)w.p;
		if (!f->proc) /* returned, poisoned under GCONVMSTRESS: a stale reference */
			break;
		markptr(f->caller);
		markvalues(f->slots, (size_t)(f->proc->nlocals + f->proc->ntemps));
		break;
	}
	case T_COEXP: {
		Coexp *c = (Coexp *)w.p;
		markptr(c->init);
		markptr(c->frame);
		markptr(c->activator);
		markptr(c->from);
		break;
	}
	case T_CSET:
		markptr(((Cset *)w.p)->str);
		break;
	case T_STRHDR:
		markptr(((String *)w.p)->s);
		break;
	case T_FILE:
		markptr(((File *)w.p)->name.s);
		break;
	case T_GCVOID: /* an array of pointers to objects */
		for (size_t i = 0; i < w.size / sizeof(void *); i++)
			markptr(((void **)w.p)[i]);
		break;
	case T_STRINGS: /* an array of Strings */
		for (size_t i = 0; i < w.size / sizeof(String); i++)
			markptr(((String *)w.p)[i].s);
		break;
	}
}

/* ---- Collecting ---- */

static void sweep(void) {
	live = 0;
	memset(freelist, 0, sizeof freelist);
	int k = 0;
	for (int ci = 0; ci < nchunks; ci++) {
		Chunk *c = chunks[ci];
		if (c->cls < 0) {
			if (!BIT(c->mark, 0)) {
				free(c->base);
				free(c->mark);
				free(c->alloc);
				free(c->type);
				free(c);
				continue;
			}
			CLR(c->mark, 0);
			live += c->size;
			chunks[k++] = c;
			continue;
		}
		for (uint32_t i = 0; i < c->used; i++) {
			char *p = c->data + (size_t)i * c->bsize;
			if (BIT(c->alloc, i) && BIT(c->mark, i)) {
				live += c->bsize;
				continue;
			}
			if (BIT(c->alloc, i)) {
				CLR(c->alloc, i);
				if (stress || debug)
					memset(p, 0xA5, c->bsize); /* poison: a use after free shows */
			}
			*(void **)p = freelist[c->cls];
			freelist[c->cls] = p;
		}
		memset(c->mark, 0, (c->nblocks + 63) / 64 * 8);
		chunks[k++] = c;
	}
	nchunks = k;
	/* Rebuild the map, without the large objects freed. */
	free(map);
	free(mapkey);
	map = NULL;
	mapkey = NULL;
	mapsize = mapcount = 0;
	for (int ci = 0; ci < nchunks; ci++)
		for (size_t off = 0; off < chunks[ci]->size; off += CHUNK)
			mapput(((uintptr_t)chunks[ci]->base + off) >> CHUNKSHIFT, chunks[ci]);
	for (int i = 0; i < NClasses; i++)
		current[i] = NULL;
	for (int ci = 0; ci < nchunks; ci++)
		if (chunks[ci]->cls >= 0 && chunks[ci]->used < chunks[ci]->nblocks)
			current[chunks[ci]->cls] = chunks[ci];
}

void frames_forget(void);

void gc_collect(void) {
	size_t before = allocated;
	frames_forget(); /* recycled frames are garbage: let the sweep have them */
	/* The roots. */
	Program *p = vm.prog;
	markvalues(vm.globals, (size_t)p->nglobals);
	markvalues(p->consts, (size_t)p->nconsts);
	markptr(vm.cur);
	markptr(vm.co);
	markptr(vm.main);
	markptr(vm.subject.s);
	markptr(vm.progname.s);
	if (vm.scratch)
		markvalues(vm.scratch, (size_t)vm.nscratch);
	markvalue(vm.errval);
	markvalue(vm.rerrval);
	markvalue(vm.eventcode);
	markvalue(vm.eventsource);
	markvalue(vm.eventvalue);
	gfx_mark();
	markptr(vm.maptab);
	markptr(vm.input);
	markptr(vm.output);
	markptr(vm.errfile);
	markptr(vm.partialfile);
	for (int i = 0; i < vm.nfiles; i++)
		markptr(vm.files[i]);
	for (int i = 0; i < nroots; i++)
		markptr(*roots[i]);
	while (nstack > 0)
		trace(stack[--nstack]);
	sweep();
	ncollections++;
	if (debug)
		fprintf(stderr, "gcon-vm: collection %lld: %zu KB allocated since the last, %zu KB live, %d chunks\n",
			(long long)ncollections, before / 1024, live / 1024, nchunks);
	allocated = 0;
	threshold = live > 16 * CHUNK ? live : 16 * CHUNK;
	gc_pending = false;
	stresscount = 0;
}

void gc_init(void) {
	for (size_t s = 0, c = 0; s <= MaxSmall / 16; s++) {
		while (classes[c] < s * 16)
			c++;
		classof[s] = (uint8_t)c;
	}
	const char *s = getenv("GCONVMSTRESS");
	if (s && atoll(s) > 0)
		stress = atoll(s);
	const char *d = getenv("GCONVMDEBUG");
	debug = d && strstr(d, "gc");
}
