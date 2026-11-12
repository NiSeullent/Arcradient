/*
 * Arcradient (MSHTML-Next)
 * base/arcr_atom.c - FNV-1a String Interning Table
 */
#include "arcr_base.h"
#include <stdlib.h>
#include <string.h>

static uint32_t fnv1a_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return (h == 0u) ? 1u : h;
}

bool arcr_atom_table_init(ArcrAtomTable *table, size_t initial_buckets) {
    if (!table) return false;
    if (initial_buckets < 64u) initial_buckets = 256u;
    arcr_arena_init(&table->arena, 16384u);
    table->buckets = (ArcrAtomEntry **)calloc(initial_buckets, sizeof(ArcrAtomEntry *));
    table->id_capacity = 256u;
    table->by_id = (ArcrAtomEntry **)calloc(table->id_capacity, sizeof(ArcrAtomEntry *));
    if (!table->buckets || !table->by_id) {
        free(table->buckets);
        free(table->by_id);
        table->buckets = NULL;
        table->by_id   = NULL;
        return false;
    }
    table->bucket_count = initial_buckets;
    table->count        = 0u; /* ID 0 reserved for ARCR_ATOM_NULL */
    return true;
}

void arcr_atom_table_destroy(ArcrAtomTable *table) {
    if (!table) return;
    free(table->buckets);
    free(table->by_id);
    arcr_arena_destroy(&table->arena);
    memset(table, 0, sizeof(*table));
}

ArcrAtom arcr_atom_intern(ArcrAtomTable *table, const char *str, size_t len) {
    if (!table || !str) return ARCR_ATOM_NULL;
    uint32_t h   = fnv1a_hash(str, len);
    size_t   idx = (size_t)h % table->bucket_count;

    for (ArcrAtomEntry *e = table->buckets[idx]; e != NULL; e = e->next) {
        if (e->hash == h && e->length == (uint32_t)len &&
            (len == 0u || memcmp(e->cstr, str, len) == 0)) {
            return e->id;
        }
    }

    ArcrAtom new_id = (ArcrAtom)(table->count + 1u);
    if ((size_t)new_id >= table->id_capacity) {
        size_t new_cap = table->id_capacity * 2u;
        ArcrAtomEntry **grown = (ArcrAtomEntry **)realloc(
            table->by_id, new_cap * sizeof(ArcrAtomEntry *));
        if (!grown) return ARCR_ATOM_NULL;
        memset(grown + table->id_capacity, 0,
               (new_cap - table->id_capacity) * sizeof(ArcrAtomEntry *));
        table->by_id       = grown;
        table->id_capacity = new_cap;
    }

    ArcrAtomEntry *entry = (ArcrAtomEntry *)arcr_arena_alloc(
        &table->arena, sizeof(ArcrAtomEntry), sizeof(void *));
    char *stored = arcr_arena_strdup(&table->arena, str, len);
    if (!entry || !stored) return ARCR_ATOM_NULL;

    entry->hash         = h;
    entry->length       = (uint32_t)len;
    entry->id           = new_id;
    entry->cstr         = stored;
    entry->next         = table->buckets[idx];
    table->buckets[idx] = entry;
    table->by_id[new_id] = entry;
    table->count++;
    return new_id;
}

ArcrAtom arcr_atom_intern_lower(ArcrAtomTable *table, const char *str, size_t len) {
    if (!table || !str) return ARCR_ATOM_NULL;
    char stack_buf[128];
    if (len < sizeof(stack_buf)) {
        for (size_t i = 0; i < len; ++i) {
            stack_buf[i] = arcr_ascii_tolower(str[i]);
        }
        return arcr_atom_intern(table, stack_buf, len);
    }
    char *tmp = (char *)malloc(len);
    if (!tmp) return ARCR_ATOM_NULL;
    for (size_t i = 0; i < len; ++i) {
        tmp[i] = arcr_ascii_tolower(str[i]);
    }
    ArcrAtom atom = arcr_atom_intern(table, tmp, len);
    free(tmp);
    return atom;
}

const char *arcr_atom_cstr(const ArcrAtomTable *table, ArcrAtom atom) {
    if (!table || atom == ARCR_ATOM_NULL || (size_t)atom > table->count) {
        return "";
    }
    return table->by_id[atom]->cstr;
}

size_t arcr_atom_len(const ArcrAtomTable *table, ArcrAtom atom) {
    if (!table || atom == ARCR_ATOM_NULL || (size_t)atom > table->count) {
        return 0u;
    }
    return (size_t)table->by_id[atom]->length;
}
