/*
 * Arcradient (MSHTML-Next)
 * base/arcr_base.h - Pure C11 Low-Level Foundations (Zero Heavy Runtime)
 */
#ifndef ARCR_BASE_H
#define ARCR_BASE_H

#include "../include/arcr_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCR_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define ARCR_MIN(a, b)    (((a) < (b)) ? (a) : (b))
#define ARCR_MAX(a, b)    (((a) > (b)) ? (a) : (b))
#define ARCR_CLAMP(x, lo, hi) ARCR_MAX((lo), ARCR_MIN((x), (hi)))

/* -------------------------------------------------------------------------
 * 1. Arena Allocator & Fixed-Size Slab Pool
 * ------------------------------------------------------------------------- */
typedef struct ArcrArenaBlock {
    struct ArcrArenaBlock *next;
    size_t                 capacity;
    size_t                 used;
    uint8_t                data[];
} ArcrArenaBlock;

typedef struct ArcrArena {
    ArcrArenaBlock *head;
    size_t          default_block_size;
    size_t          total_allocated;
    size_t          total_used;
} ArcrArena;

void  arcr_arena_init(ArcrArena *arena, size_t default_block_size);
void *arcr_arena_alloc(ArcrArena *arena, size_t size, size_t align);
void *arcr_arena_calloc(ArcrArena *arena, size_t count, size_t elem_size, size_t align);
char *arcr_arena_strdup(ArcrArena *arena, const char *str, size_t len);
void  arcr_arena_reset(ArcrArena *arena);   /* Retains first block for reuse */
void  arcr_arena_destroy(ArcrArena *arena); /* Releases all OS heap blocks immediately */

typedef struct ArcrSlabFreeNode {
    struct ArcrSlabFreeNode *next;
} ArcrSlabFreeNode;

typedef struct ArcrSlabPool {
    ArcrArena        *backing_arena;
    ArcrSlabFreeNode *free_list;
    size_t            slot_size;
    size_t            slot_align;
    size_t            active_count;
} ArcrSlabPool;

void  arcr_slab_init(ArcrSlabPool *pool, ArcrArena *backing, size_t slot_size, size_t align);
void *arcr_slab_alloc(ArcrSlabPool *pool);
void  arcr_slab_free(ArcrSlabPool *pool, void *ptr);

/* -------------------------------------------------------------------------
 * 2. Interned String Table (ArcrAtom)
 * ------------------------------------------------------------------------- */
typedef uint32_t ArcrAtom;
#define ARCR_ATOM_NULL 0u

typedef struct ArcrAtomEntry {
    uint32_t              hash;
    uint32_t              length;
    ArcrAtom              id;
    const char           *cstr;
    struct ArcrAtomEntry *next;
} ArcrAtomEntry;

typedef struct ArcrAtomTable {
    ArcrArena       arena;
    ArcrAtomEntry **buckets;
    size_t          bucket_count;
    ArcrAtomEntry **by_id;
    size_t          id_capacity;
    size_t          count;
} ArcrAtomTable;

bool        arcr_atom_table_init(ArcrAtomTable *table, size_t initial_buckets);
void        arcr_atom_table_destroy(ArcrAtomTable *table);
ArcrAtom    arcr_atom_intern(ArcrAtomTable *table, const char *str, size_t len);
ArcrAtom    arcr_atom_intern_lower(ArcrAtomTable *table, const char *str, size_t len);
const char *arcr_atom_cstr(const ArcrAtomTable *table, ArcrAtom atom);
size_t      arcr_atom_len(const ArcrAtomTable *table, ArcrAtom atom);

/* -------------------------------------------------------------------------
 * 3. Safe String Utilities (Win2000 msvcrt-safe)
 * ------------------------------------------------------------------------- */
size_t arcr_str_copy(char *dst, size_t dst_cap, const char *src);
bool   arcr_str_eq_nocase_n(const char *a, size_t a_len, const char *b, size_t b_len);
bool   arcr_str_eq_nocase(const char *a, const char *b);
bool   arcr_str_starts_with_nocase(const char *str, const char *prefix);
void   arcr_str_trim_view(const char **inout_ptr, size_t *inout_len);
bool   arcr_parse_i32(const char *s, size_t len, int32_t *out_val);
char   arcr_ascii_tolower(char c);

/* -------------------------------------------------------------------------
 * 4. UTF-8, UTF-16LE & Korean / CJK Line-Break Rules
 * ------------------------------------------------------------------------- */
#define ARCR_UNICODE_REPLACEMENT 0xFFFDu

size_t arcr_utf8_decode_one(const char *s, size_t len, uint32_t *out_codepoint);
size_t arcr_utf8_encode_one(uint32_t codepoint, char out_buf[4]);
size_t arcr_utf8_to_utf16le(const char *utf8, size_t utf8_len,
                            uint16_t *out_utf16, size_t out_cap_units);

bool arcr_cp_is_hangul(uint32_t cp);
bool arcr_cp_is_cjk_ideograph(uint32_t cp);
bool arcr_cp_is_cjk_no_line_start(uint32_t cp); /* 행두 금칙 문자: ., ) ] ” 등 */
bool arcr_cp_is_cjk_no_line_end(uint32_t cp);   /* 행말 금칙 문자: ( [ “ 등 */
bool arcr_cp_is_whitespace(uint32_t cp);

/*
 * Determines if a line wrap is allowed immediately between `prev_cp` and `next_cp`.
 * - Respects Korean 어절 (whitespace) and Hangul syllable boundaries.
 * - When `keep_all` is true (`word-break: keep-all`), prevents breaking inside
 *   consecutive Hangul syllables while still allowing breaks at spaces/hyphens.
 * - Always enforces CJK 행두·행말 금칙처리 so punctuation is never orphaned!
 */
bool arcr_unicode_can_break_line(uint32_t prev_cp, uint32_t next_cp, bool keep_all);

/* -------------------------------------------------------------------------
 * 5. Runtime CPUID SIMD Capability Detection
 * ------------------------------------------------------------------------- */
uint32_t    arcr_detect_cpu_flags(void);
const char *arcr_cpu_simd_tier_name(uint32_t flags);

#endif /* ARCR_BASE_H */
