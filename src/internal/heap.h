#ifndef SRC_INTERNAL_HEAP_H
#define SRC_INTERNAL_HEAP_H
/* src/internal/heap.h - malloc()'s small blocks, carved from chunks (heap.c).
**
** __hpget() returns a block of at least size bytes, or NULL when the heap
** cannot serve it and malloc() should GETMAIN as before.  __hpput() takes
** back a block __hpget() gave, returning 1, or returns 0 for any other
** block, which free() then FREEMAINs.  __hprel() gives a program's chunks
** back at its end (__grtres), those with no block still out.
*/
#include <stddef.h>
#include "mvs/crt.h"

#define HEAP_MAXSIZE    (2048 - 8)      /* largest request it serves    */

void    *__hpget(size_t size);
int     __hpput(void *ptr);
void    __hprel(CLIBGRT *grt);

#endif /* SRC_INTERNAL_HEAP_H */
