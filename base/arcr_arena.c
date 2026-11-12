/*
 * Arcradient (MSHTML-Next)
 * base/arcr_arena.c - Arena Bump Allocator, Slab Pool & Module ABI Validator
 */
#include "arcr_base.h"
#include <stdlib.h>
#include <string.h>

static uintptr_t align_up_uintptr(uintptr_t ptr, size_t align) {
    size_t mask = align - 1u;
    return (ptr + mask) & ~(uintptr_t)mask;
}

void arcr_arena_init(ArcrArena *arena, size_t default_block_size) {
    if (!arena) return;
    arena->head               = NULL;
    arena->default_block_size = (default_block_size >= 1024u) ? default_block_size : 16384u;
    arena->total_allocated    = 0u;
    arena->total_used         = 0u;
}

void *arcr_arena_alloc(ArcrArena *arena, size_t size, size_t align) {
    if (!arena || size == 0u) return NULL;
    if (align == 0u || (align & (align - 1u)) != 0u) {
        align = sizeof(void *);
    }

    ArcrArenaBlock *blk = arena->head;
    if (blk) {
        uintptr_t base    = (uintptr_t)blk->data;
        uintptr_t cur     = base + blk->used;
        uintptr_t aligned = align_up_uintptr(cur, align);
        size_t    new_used = (size_t)(aligned - base) + size;
        if (new_used <= blk->capacity) {
            arena->total_used += (new_used - blk->used);
            blk->used = new_used;
            return (void *)aligned;
        }
    }

    size_t cap = arena->default_block_size;
    if (size + align > cap) {
        cap = size + align;
    }
    ArcrArenaBlock *new_blk = (ArcrArenaBlock *)malloc(sizeof(ArcrArenaBlock) + cap);
    if (!new_blk) return NULL;

    new_blk->capacity = cap;
    new_blk->used     = 0u;
    new_blk->next     = arena->head;
    arena->head       = new_blk;
    arena->total_allocated += sizeof(ArcrArenaBlock) + cap;

    uintptr_t base    = (uintptr_t)new_blk->data;
    uintptr_t aligned = align_up_uintptr(base, align);
    size_t    used    = (size_t)(aligned - base) + size;
    new_blk->used     = used;
    arena->total_used += used;
    return (void *)aligned;
}

void *arcr_arena_calloc(ArcrArena *arena, size_t count, size_t elem_size, size_t align) {
    if (count == 0u || elem_size == 0u) return NULL;
    if (count > (SIZE_MAX / elem_size)) return NULL;
    size_t total = count * elem_size;
    void  *p     = arcr_arena_alloc(arena, total, align);
    if (p) memset(p, 0, total);
    return p;
}

char *arcr_arena_strdup(ArcrArena *arena, const char *str, size_t len) {
    if (!arena) return NULL;
    char *copy = (char *)arcr_arena_alloc(arena, len + 1u, 1u);
    if (!copy) return NULL;
    if (str && len > 0u) {
        memcpy(copy, str, len);
    }
    copy[len] = '\0';
    return copy;
}

void arcr_arena_reset(ArcrArena *arena) {
    if (!arena || !arena->head) return;
    /* Free all blocks except the head block so quick reuse avoids malloc */
    ArcrArenaBlock *keep = arena->head;
    ArcrArenaBlock *curr = keep->next;
    while (curr) {
        ArcrArenaBlock *next = curr->next;
        free(curr);
        curr = next;
    }
    keep->next             = NULL;
    keep->used             = 0u;
    arena->total_allocated = sizeof(ArcrArenaBlock) + keep->capacity;
    arena->total_used      = 0u;
}

void arcr_arena_destroy(ArcrArena *arena) {
    if (!arena) return;
    ArcrArenaBlock *curr = arena->head;
    while (curr) {
        ArcrArenaBlock *next = curr->next;
        free(curr);
        curr = next;
    }
    arena->head            = NULL;
    arena->total_allocated = 0u;
    arena->total_used      = 0u;
}

void arcr_slab_init(ArcrSlabPool *pool, ArcrArena *backing, size_t slot_size, size_t align) {
    if (!pool) return;
    pool->backing_arena = backing;
    pool->free_list     = NULL;
    pool->slot_size     = (slot_size >= sizeof(ArcrSlabFreeNode)) ? slot_size : sizeof(ArcrSlabFreeNode);
    pool->slot_align    = (align >= sizeof(void *)) ? align : sizeof(void *);
    pool->active_count  = 0u;
}

void *arcr_slab_alloc(ArcrSlabPool *pool) {
    if (!pool) return NULL;
    void *res = NULL;
    if (pool->free_list) {
        ArcrSlabFreeNode *node = pool->free_list;
        pool->free_list = node->next;
        res = (void *)node;
        memset(res, 0, pool->slot_size);
    } else {
        res = arcr_arena_calloc(pool->backing_arena, 1u, pool->slot_size, pool->slot_align);
    }
    if (res) pool->active_count++;
    return res;
}

void arcr_slab_free(ArcrSlabPool *pool, void *ptr) {
    if (!pool || !ptr) return;
    ArcrSlabFreeNode *node = (ArcrSlabFreeNode *)ptr;
    node->next = pool->free_list;
    pool->free_list = node;
    if (pool->active_count > 0u) pool->active_count--;
}

/* -------------------------------------------------------------------------
 * Versioned Module ABI Validation
 * ------------------------------------------------------------------------- */
ArcrModuleStatus arcr_module_validate(
    const ArcrModuleDescriptor *desc,
    ArcrArchId                  host_arch,
    uint16_t                    host_os_version,
    uint32_t                    host_cpu_flags
) {
    if (!desc) return ARCR_MOD_ERR_NULL;
    if (desc->magic != ARCR_MODULE_MAGIC) return ARCR_MOD_ERR_BAD_MAGIC;
    if (desc->struct_size != sizeof(ArcrModuleDescriptor)) return ARCR_MOD_ERR_BAD_STRUCT_SIZE;
    if (desc->abi_version != ARCR_ABI_VERSION) return ARCR_MOD_ERR_ABI_MISMATCH;
    if (desc->engine_build_id != ARCR_ENGINE_BUILD_ID) return ARCR_MOD_ERR_BUILD_MISMATCH;
    if ((ArcrArchId)desc->arch_id != host_arch) return ARCR_MOD_ERR_ARCH_MISMATCH;
    if (host_os_version < desc->min_os_major_minor) return ARCR_MOD_ERR_OS_TOO_OLD;
    if ((host_cpu_flags & desc->required_cpu_flags) != desc->required_cpu_flags) {
        return ARCR_MOD_ERR_CPU_UNSUPPORTED;
    }
    if (!desc->vtable) return ARCR_MOD_ERR_MISSING_VTABLE;
    return ARCR_MOD_OK;
}

const char *arcr_module_status_str(ArcrModuleStatus status) {
    switch (status) {
        case ARCR_MOD_OK:                  return "OK";
        case ARCR_MOD_ERR_NULL:            return "NULL module descriptor";
        case ARCR_MOD_ERR_BAD_MAGIC:       return "Invalid module magic";
        case ARCR_MOD_ERR_BAD_STRUCT_SIZE: return "Descriptor struct_size mismatch";
        case ARCR_MOD_ERR_ABI_MISMATCH:    return "Module ABI version mismatch";
        case ARCR_MOD_ERR_BUILD_MISMATCH:  return "Engine build ID mismatch";
        case ARCR_MOD_ERR_ARCH_MISMATCH:   return "CPU architecture mismatch (x86 vs x64)";
        case ARCR_MOD_ERR_OS_TOO_OLD:      return "Host OS version below module requirement";
        case ARCR_MOD_ERR_CPU_UNSUPPORTED: return "Host CPU lacks required SIMD instructions";
        case ARCR_MOD_ERR_MISSING_VTABLE:  return "Module vtable is NULL";
        default:                           return "Unknown module error";
    }
}
