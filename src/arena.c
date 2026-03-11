/*
 * arena.c — bump-pointer arena allocator
 *
 * Memory is allocated in large blocks; individual allocations are never
 * freed.  Call arena_free() to release everything at once.
 */

#include "ast.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define DEFAULT_BLOCK_SIZE  (1u << 20)   /* 1 MiB */
#define ALIGN_UP(n, a)      (((n) + (a) - 1) & ~((a) - 1))
#define PTR_ALIGN           (sizeof(void *))

typedef struct Block {
    struct Block *prev;
    size_t        cap;
    size_t        used;
    /* data[] follows immediately */
} Block;

struct Arena {
    Block  *cur;
    size_t  block_size;
    size_t  total;
};

Arena *arena_new(size_t block_size)
{
    if (block_size == 0)
        block_size = DEFAULT_BLOCK_SIZE;
    Arena *a = (Arena *)malloc(sizeof(Arena));
    if (!a) { perror("arena_new"); exit(1); }
    a->block_size = block_size;
    a->cur        = NULL;
    a->total      = 0;
    return a;
}

void arena_free(Arena *a)
{
    Block *b = a->cur;
    while (b) {
        Block *prev = b->prev;
        free(b);
        b = prev;
    }
    free(a);
}

void *arena_alloc(Arena *a, size_t nbytes)
{
    nbytes = ALIGN_UP(nbytes, PTR_ALIGN);
    if (nbytes == 0) nbytes = PTR_ALIGN;

    if (!a->cur || a->cur->used + nbytes > a->cur->cap) {
        size_t bsz = a->block_size;
        if (nbytes > bsz) bsz = nbytes + PTR_ALIGN;
        Block *b = (Block *)malloc(sizeof(Block) + bsz);
        if (!b) { perror("arena_alloc"); exit(1); }
        b->prev  = a->cur;
        b->cap   = bsz;
        b->used  = 0;
        a->cur   = b;
    }

    char *p = (char *)(a->cur + 1) + a->cur->used;
    a->cur->used += nbytes;
    a->total     += nbytes;
    memset(p, 0, nbytes);
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *p = (char *)arena_alloc(a, n);
    memcpy(p, s, n);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    if (!s) return NULL;
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
