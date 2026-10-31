/* HEAP.C - malloc()'s small blocks, carved from chunks.
**
** malloc() was one GETMAIN and free() one FREEMAIN, SVCs of some 600
** instructions each.  A program that makes and drops many small strings
** -- BREXX makes thousands for one exec -- spent a third of its time in
** them.  Here a request up to HEAP_MAXSIZE takes a block from a free list,
** or carves it from a chunk of CHUNK_SIZE that one GETMAIN got; free() puts
** it back on its list.  Larger requests GETMAIN as before.
**
** A block is exactly what @@GETM would have got: the request and the 8
** byte prefix rounded up to 64 bytes.  So a caller that writes a little
** past what it asked for, into the rounding, still writes into its own
** block, as it always has -- smaller blocks than that made such callers
** overwrite their neighbours.  The free lists are by that size, 64 to
** 2048 bytes, and every size carves from the same chunk.
**
** The block keeps @@GETM's prefix, so what reads it still works:
**   +0  chunk address | 1  (a GETMAIN'd length is a multiple of 64, so
**                           bit 31 tells the two apart for free())
**   +4  size requested by the caller, as @@GETM stores it (realloc(),
**       and BREXX's heapSize(), read this word; the block's size follows
**       from it, so a block goes back to the right list)
**
** One heap for each program (CLIBGRT, so it goes with the program, found
** as @@GETM finds the heap subpool: PSATOLD -> TCB -> TCBFSAB -> the PPA),
** used only by the task that owns that PPA.  A cthread TCB has no PPA and
** GETMAINs as before.  Only when the ambient heap subpool is 0: blocks
** in any other subpool GETMAIN, so they travel with their subpool as
** __setsp() promises.  A chunk is never got in supervisor state, where
** subpool 0 is subpool 252, key 0, and the blocks would not be the
** program's to write.  A block may be freed by another task: it goes on
** the heap's remote list, with CS, and the owner takes it from there.
** At the program's end, __hprel() FREEMAINs every chunk with nothing out.
*/
#define STDLIB_C
#include "stdlib.h"
#include "string.h"
#include "stddef.h"
#include "mvs/storage.h"
#include "mvs/crt.h"
#include "mvs/apf.h"
#include "s370/atomic.h"
#include "src/internal/heap.h"

#define HEAP_CLASSES    32              /* 64, 128, ... 2048 bytes      */
#define CHUNK_SIZE      (32 * 1024)
#define CHUNK_HEAD      64              /* blocks start here            */

typedef struct heap     HEAP;
typedef struct heapchk  HEAPCHK;

struct heapchk {
    char        eye[4];                 /* "HCHK"                       */
    HEAP        *heap;                  /* the heap it belongs to       */
    HEAPCHK     *next;                  /* the heap's other chunks      */
    unsigned    live;                   /* blocks out                   */
    unsigned    carve;                  /* offset of the next new block */
};

struct heap {
    char        eye[4];                 /* "HEAP"                       */
    void        *tcb;                   /* owner, 0 once released       */
    HEAPCHK     *chunks;                /* all of them, the newest first */
    void        *free[HEAP_CLASSES];    /* free blocks, by size         */
    unsigned    remote;                 /* freed by other tasks         */
};

/* a free block links to the next through the caller's first word */
#define NEXT(p)         (*(void **)(p))
#define PREFIX(p, n)    (((unsigned *)(p))[(n) - 2])
#define CHUNK(p)        ((HEAPCHK *)(PREFIX(p, 0) & 0xFFFFFFF8))
/* the size class of a request: its block is (class + 1) * 64 bytes */
#define CLASSOF(size)   (((size) + 8 + 63) / 64 - 1)
#define BLKSIZE(c)      (((c) + 1) * 64)

static void *curtcb(void)
{
    unsigned    *psa = 0;                       /* low core == PSA      */
    return (void *)psa[0x21C / 4];              /* PSATOLD              */
}

/* This TCB's own PPA, as @@GETM and heapsp.c find it, or NULL */
static CLIBPPA *curppa(void)
{
    unsigned    tcb = (unsigned)curtcb();
    unsigned    fsa;
    unsigned    ppa;

    if (!tcb) return (CLIBPPA *)0;
    fsa = *(unsigned *)(tcb + 0x70) & 0x00FFFFFF;   /* TCBFSAB (AL3)    */
    if (!fsa) return (CLIBPPA *)0;
    ppa = *(unsigned *)(fsa + 8);               /* "next" save area     */
    if (!ppa || ppa > 0x00FFFFFF) return (CLIBPPA *)0;
    if (memcmp(((CLIBPPA *)ppa)->ppaeye, PPAEYE, 4) != 0) return (CLIBPPA *)0;
    return (CLIBPPA *)ppa;
}

/* What other tasks freed, onto the free lists */
static void drain(HEAP *h)
{
    void        *p = (void *)__swap(&h->remote, 0);

    while (p) {
        void        *next = NEXT(p);
        unsigned    c = CLASSOF(PREFIX(p, 1));

        CHUNK(p)->live--;
        NEXT(p) = h->free[c];
        h->free[c] = p;
        p = next;
    }
}

static HEAPCHK *newchunk(HEAP *h)
{
    HEAPCHK     *k;

    if (__issup()) return (HEAPCHK *)0;
    k = __getm(CHUNK_SIZE);
    if (!k) return (HEAPCHK *)0;
    memcpy(k->eye, "HCHK", 4);
    k->heap = h;
    k->live = 0;
    k->carve = CHUNK_HEAD;
    k->next = h->chunks;
    h->chunks = k;
    return k;
}

__asm__("\n&FUNC    SETC '__hpget'");
void *__hpget(size_t size)
{
    CLIBPPA     *ppa;
    CLIBGRT     *grt;
    HEAP        *h;
    HEAPCHK     *k;
    unsigned    c;
    unsigned    *blk;
    void        *p;

    if (size == 0 || size > HEAP_MAXSIZE) return (void *)0;
    ppa = curppa();
    if (!ppa || ppa->ppaheaps != 0) return (void *)0;
    grt = ppa->ppagrt;
    if (!grt) return (void *)0;
    h = grt->grtheap;
    if (!h) {
        if (__issup()) return (void *)0;
        h = __getm(sizeof(HEAP));
        if (!h) return (void *)0;
        memset(h, 0, sizeof(HEAP));
        memcpy(h->eye, "HEAP", 4);
        h->tcb = curtcb();
        grt->grtheap = h;
    }
    if (h->tcb != curtcb()) return (void *)0;

    c = CLASSOF(size);
    if (!h->free[c] && h->remote) drain(h);
    p = h->free[c];
    if (p) {
        h->free[c] = NEXT(p);
        CHUNK(p)->live++;
        PREFIX(p, 1) = size;
        return p;
    }

    k = h->chunks;
    if (!k || k->carve + BLKSIZE(c) > CHUNK_SIZE) {
        k = newchunk(h);
        if (!k) return (void *)0;
    }
    blk = (unsigned *)((char *)k + k->carve);
    k->carve += BLKSIZE(c);
    k->live++;
    blk[0] = (unsigned)k | 1;
    blk[1] = size;
    return blk + 2;
}

__asm__("\n&FUNC    SETC '__hpput'");
int __hpput(void *ptr)
{
    HEAPCHK     *k;
    HEAP        *h;
    unsigned    c;
    unsigned    old;

    /* without the high bit, which a list's last pointer carries: SVC 99
       text units are freed with it on (__frtx9a), and FREEMAIN, which
       ignored it, never minded -- a block handed out again with it
       ended the next list early */
    ptr = (void *)((unsigned)ptr & 0x7FFFFFFF);
    if (!(PREFIX(ptr, 0) & 1)) return 0;        /* GETMAIN'd: not ours  */
    k = CHUNK(ptr);
    h = k->heap;
    if (h->tcb == curtcb()) {
        c = CLASSOF(PREFIX(ptr, 1));
        k->live--;
        NEXT(ptr) = h->free[c];
        h->free[c] = ptr;
        return 1;
    }
    old = h->remote;
    for (;;) {
        NEXT(ptr) = (void *)old;
        if (__cas(&h->remote, &old, (unsigned)ptr) == 0) break;
    }
    return 1;
}

__asm__("\n&FUNC    SETC '__hprel'");
void __hprel(CLIBGRT *grt)
{
    HEAP        *h = grt->grtheap;
    HEAPCHK     **kp;
    HEAPCHK     *k;

    if (!h) return;
    drain(h);
    for (kp = &h->chunks; (k = *kp) != 0; ) {
        if (k->live == 0) {
            *kp = k->next;
            memset(k->eye, 0, 4);
            __freem(k);
        } else {
            kp = &k->next;
        }
    }
    grt->grtheap = 0;
    if (!h->chunks) {
        memset(h->eye, 0, 4);
        __freem(h);
    } else {
        h->tcb = 0;         /* blocks still out: their frees go remote  */
    }
}
