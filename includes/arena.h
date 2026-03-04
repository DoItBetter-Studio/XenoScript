/*
 * arena.h — A simple bump-pointer arena allocator
 *
 * The compiler allocates hundreds to thousands of small AST nodes during
 * parsing. Using malloc/free per node would be:
 *   - Slow (many small allocations)
 *   - Error-prone (easy to leak or double-free)
 *   - Unnecessary (we never need to free individual nodes — we free everything
 *     at once when compilation finishes)
 *
 * An arena solves all three: one big malloc upfront, bump a pointer for each
 * allocation, free the whole block at the end. O(1) alloc, zero fragmentation.
 *
 * Since this is a dev-side tool (not the runtime VM), a 1MB arena is plenty
 * for any realistic script file. If you ever need more, you can chain arenas.
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>   /* size_t        */
#include <stdint.h>   /* uint8_t       */
#include <stdlib.h>   /* malloc, free  */
#include <string.h>   /* memset        */

#define ARENA_DEFAULT_SIZE (1024 * 1024)  /* 1 MB — more than enough for scripts */

typedef struct {
    uint8_t *base;    /* Start of the memory block                     */
    size_t   size;    /* Total capacity in bytes                       */
    size_t   offset;  /* How many bytes have been allocated so far     */
} Arena;

/* Initialize the arena with a fresh heap allocation.
 * Returns 0 on success, -1 if malloc fails. */
static inline int arena_init(Arena *arena, size_t size) {
    arena->base   = (uint8_t *)malloc(size);
    if (!arena->base) return -1;
    arena->size   = size;
    arena->offset = 0;
    return 0;
}

/* Allocate `size` bytes from the arena, aligned to pointer size.
 * Returns NULL if the arena is exhausted.
 *
 * Alignment: we align to sizeof(void*) to ensure all allocated structs
 * are naturally aligned on the platform. Without this, a struct allocated
 * after an odd-sized one could be misaligned, causing UB on strict-alignment
 * architectures. */
static inline void *arena_alloc(Arena *arena, size_t size) {
    /* Round up to the next pointer-aligned boundary */
    size_t align    = sizeof(void *);
    size_t aligned  = (size + align - 1) & ~(align - 1);

    if (arena->offset + aligned > arena->size) return NULL;  /* out of space */

    void *ptr       = arena->base + arena->offset;
    arena->offset  += aligned;
    memset(ptr, 0, aligned);  /* zero-initialize so we never read garbage */
    return ptr;
}

/* Free all arena memory at once.
 * This is the only "free" operation — individual allocations cannot be freed. */
static inline void arena_free(Arena *arena) {
    free(arena->base);
    arena->base   = NULL;
    arena->size   = 0;
    arena->offset = 0;
}

/* How many bytes have been used so far — useful for diagnostics */
static inline size_t arena_used(const Arena *arena) {
    return arena->offset;
}

#endif /* ARENA_H */