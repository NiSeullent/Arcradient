/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * tools/pack_installer.c - Pure C11 Self-Extracting Installer (WSSFX100) Packer
 *
 * Usage:
 *   pack_installer <setup_stub_exe> <staging_dir> <output_setup_exe>
 *
 * Recursively collects all files in <staging_dir> with normalized forward-slash
 * relative paths, compresses them into a standard PKZIP archive via `arcr_zip_create_mem`,
 * appends the archive + 32-byte `WSSFX100` trailer onto <setup_stub_exe>, and
 * performs full read-back CRC-32 + entry extraction verification before exiting 0.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../base/arcr_codec.h"
#include "../installer/win32/waterscape_sfx_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

typedef struct StagedFileItem {
    char     rel_path[128];
    char     full_path[1024];
    uint8_t *data;
    size_t   size;
} StagedFileItem;

typedef struct StagedFileList {
    StagedFileItem *items;
    size_t          count;
    size_t          capacity;
} StagedFileList;

static bool read_entire_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    size_t alloc_sz = (sz > 0) ? (size_t)sz : 1u;
    uint8_t *buf = (uint8_t *)malloc(alloc_sz);
    if (!buf) {
        fclose(fp);
        return false;
    }
    if (sz > 0) {
        size_t rd = fread(buf, 1u, (size_t)sz, fp);
        if (rd != (size_t)sz) {
            free(buf);
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return true;
}

static bool staged_list_push(StagedFileList *list, const char *rel_path, const char *full_path) {
    size_t rlen = strlen(rel_path);
    size_t flen = strlen(full_path);
    if (rlen == 0u || rlen >= 128u) {
        fprintf(stderr, "pack_installer: staged relative path too long (%lu >= 128): '%s'\n",
                (unsigned long)rlen, rel_path);
        return false;
    }
    if (flen == 0u || flen >= 1024u) {
        fprintf(stderr, "pack_installer: staged full path too long: '%s'\n", full_path);
        return false;
    }

    if (list->count >= list->capacity) {
        size_t ncap = (list->capacity == 0u) ? 32u : (list->capacity * 2u);
        StagedFileItem *grown = (StagedFileItem *)realloc(list->items, ncap * sizeof(StagedFileItem));
        if (!grown) return false;
        list->items    = grown;
        list->capacity = ncap;
    }

    StagedFileItem *it = &list->items[list->count];
    memset(it, 0, sizeof(*it));
    for (size_t i = 0u; i < rlen; ++i) {
        it->rel_path[i] = (rel_path[i] == '\\') ? '/' : rel_path[i];
    }
    it->rel_path[rlen] = '\0';
    memcpy(it->full_path, full_path, flen + 1u);
    list->count++;
    return true;
}

static void staged_list_free(StagedFileList *list) {
    if (!list) return;
    for (size_t i = 0u; i < list->count; ++i) {
        free(list->items[i].data);
    }
    free(list->items);
    list->items    = NULL;
    list->count    = 0u;
    list->capacity = 0u;
}

static int compare_staged_items(const void *a, const void *b) {
    const StagedFileItem *ia = (const StagedFileItem *)a;
    const StagedFileItem *ib = (const StagedFileItem *)b;
    return strcmp(ia->rel_path, ib->rel_path);
}

static bool collect_staging_recursive(const char *root_dir,
                                      const char *rel_prefix,
                                      StagedFileList *list) {
#if defined(_WIN32)
    char pattern[1024];
    int pn = snprintf(pattern, sizeof(pattern), "%s\\*", root_dir);
    if (pn <= 0 || (size_t)pn >= sizeof(pattern)) return false;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char full_sub[1024];
        char rel_sub[256];
        int fn = snprintf(full_sub, sizeof(full_sub), "%s\\%s", root_dir, name);
        int rn = (rel_prefix[0] != '\0')
            ? snprintf(rel_sub, sizeof(rel_sub), "%s/%s", rel_prefix, name)
            : snprintf(rel_sub, sizeof(rel_sub), "%s", name);
        if (fn <= 0 || (size_t)fn >= sizeof(full_sub) ||
            rn <= 0 || (size_t)rn >= sizeof(rel_sub)) {
            FindClose(h);
            return false;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            if (!collect_staging_recursive(full_sub, rel_sub, list)) {
                FindClose(h);
                return false;
            }
        } else {
            if (!staged_list_push(list, rel_sub, full_sub)) {
                FindClose(h);
                return false;
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return true;
#else
    DIR *dir = opendir(root_dir);
    if (!dir) return false;

    struct dirent *de = NULL;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char full_sub[1024];
        char rel_sub[256];
        int fn = snprintf(full_sub, sizeof(full_sub), "%s/%s", root_dir, name);
        int rn = (rel_prefix[0] != '\0')
            ? snprintf(rel_sub, sizeof(rel_sub), "%s/%s", rel_prefix, name)
            : snprintf(rel_sub, sizeof(rel_sub), "%s", name);
        if (fn <= 0 || (size_t)fn >= sizeof(full_sub) ||
            rn <= 0 || (size_t)rn >= sizeof(rel_sub)) {
            closedir(dir);
            return false;
        }

        struct stat st;
        if (stat(full_sub, &st) != 0) {
            closedir(dir);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!collect_staging_recursive(full_sub, rel_sub, list)) {
                closedir(dir);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!staged_list_push(list, rel_sub, full_sub)) {
                closedir(dir);
                return false;
            }
        }
    }

    closedir(dir);
    return true;
#endif
}

static bool verify_packed_installer(const char *output_exe,
                                    const StagedFileList *expected_list) {
    uint8_t *exe_bytes = NULL;
    size_t   exe_len   = 0u;
    if (!read_entire_file(output_exe, &exe_bytes, &exe_len)) {
        fprintf(stderr, "pack_installer: verification failed reading '%s'\n", output_exe);
        return false;
    }

    if (exe_len < WS_SFX_TRAILER_BYTES) {
        fprintf(stderr, "pack_installer: output smaller than trailer (%lu bytes)\n",
                (unsigned long)exe_len);
        free(exe_bytes);
        return false;
    }

    WsSfxTrailer tr;
    if (!ws_sfx_trailer_decode(exe_bytes + (exe_len - WS_SFX_TRAILER_BYTES), &tr) ||
        !ws_sfx_trailer_validate_bounds(&tr, (uint64_t)exe_len)) {
        fprintf(stderr, "pack_installer: WSSFX100 trailer bounds invalid\n");
        free(exe_bytes);
        return false;
    }

    if (tr.file_count != (uint32_t)expected_list->count) {
        fprintf(stderr, "pack_installer: file_count mismatch (%lu vs %lu)\n",
                (unsigned long)tr.file_count, (unsigned long)expected_list->count);
        free(exe_bytes);
        return false;
    }

    const uint8_t *zip_ptr = exe_bytes + (size_t)tr.zip_offset;
    size_t         zip_len = (size_t)tr.zip_size;
    uint32_t       actual_crc = arcr_crc32(zip_ptr, zip_len);
    if (actual_crc != tr.zip_crc32) {
        fprintf(stderr, "pack_installer: ZIP CRC-32 mismatch (0x%08lX vs 0x%08lX)\n",
                (unsigned long)actual_crc, (unsigned long)tr.zip_crc32);
        free(exe_bytes);
        return false;
    }

    char (*listed_names)[128] = (char (*)[128])calloc(
        expected_list->count > 0u ? expected_list->count : 1u, sizeof(*listed_names));
    if (!listed_names) {
        free(exe_bytes);
        return false;
    }

    size_t listed = arcr_zip_list_entries(zip_ptr, zip_len, listed_names, expected_list->count);
    if (listed != expected_list->count) {
        fprintf(stderr, "pack_installer: ZIP listed entry count mismatch (%lu vs %lu)\n",
                (unsigned long)listed, (unsigned long)expected_list->count);
        free(listed_names);
        free(exe_bytes);
        return false;
    }

    for (size_t i = 0u; i < listed; ++i) {
        if (strcmp(listed_names[i], expected_list->items[i].rel_path) != 0) {
            fprintf(stderr, "pack_installer: entry[%lu] mismatch ('%s' vs '%s')\n",
                    (unsigned long)i, listed_names[i], expected_list->items[i].rel_path);
            free(listed_names);
            free(exe_bytes);
            return false;
        }
    }

    /* Spot-extract first and last entries to confirm DEFLATE round-trip integrity */
    if (listed > 0u) {
        size_t indices[2] = {0u, listed - 1u};
        for (size_t k = 0u; k < 2u; ++k) {
            size_t idx = indices[k];
            uint8_t *ext = NULL;
            size_t   ext_len = 0u;
            if (!arcr_zip_extract_entry(zip_ptr, zip_len, listed_names[idx], &ext, &ext_len) ||
                ext_len != expected_list->items[idx].size ||
                (ext_len > 0u && memcmp(ext, expected_list->items[idx].data, ext_len) != 0)) {
                fprintf(stderr, "pack_installer: extraction verification failed for '%s'\n",
                        listed_names[idx]);
                free(ext);
                free(listed_names);
                free(exe_bytes);
                return false;
            }
            free(ext);
        }
    }

    free(listed_names);
    free(exe_bytes);
    return true;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: %s <setup_stub_exe> <staging_dir> <output_setup_exe>\n",
               argv[0] ? argv[0] : "pack_installer");
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <setup_stub_exe> <staging_dir> <output_setup_exe>\n",
                (argc > 0 && argv[0]) ? argv[0] : "pack_installer");
        return 1;
    }

    const char *stub_exe_path   = argv[1];
    const char *staging_dir     = argv[2];
    const char *output_exe_path = argv[3];

    uint8_t *stub_bytes = NULL;
    size_t   stub_len   = 0u;
    if (!read_entire_file(stub_exe_path, &stub_bytes, &stub_len) || stub_len == 0u) {
        fprintf(stderr, "pack_installer: failed to read setup stub '%s'\n", stub_exe_path);
        free(stub_bytes);
        return 1;
    }

    /* Strip trailing slash on staging_dir if present */
    char clean_stage[1024];
    size_t slen = strlen(staging_dir);
    if (slen == 0u || slen >= sizeof(clean_stage)) {
        fprintf(stderr, "pack_installer: invalid staging directory '%s'\n", staging_dir);
        free(stub_bytes);
        return 1;
    }
    memcpy(clean_stage, staging_dir, slen + 1u);
    while (slen > 1u && (clean_stage[slen - 1u] == '/' || clean_stage[slen - 1u] == '\\')) {
        clean_stage[--slen] = '\0';
    }

    StagedFileList list = {NULL, 0u, 0u};
    if (!collect_staging_recursive(clean_stage, "", &list) || list.count == 0u) {
        fprintf(stderr, "pack_installer: no files collected from staging directory '%s'\n",
                clean_stage);
        staged_list_free(&list);
        free(stub_bytes);
        return 1;
    }

    qsort(list.items, list.count, sizeof(StagedFileItem), compare_staged_items);

    ArcrZipEntryView *views = (ArcrZipEntryView *)calloc(list.count, sizeof(ArcrZipEntryView));
    if (!views) {
        staged_list_free(&list);
        free(stub_bytes);
        return 1;
    }

    size_t raw_total_bytes = 0u;
    for (size_t i = 0u; i < list.count; ++i) {
        if (!read_entire_file(list.items[i].full_path,
                              &list.items[i].data,
                              &list.items[i].size)) {
            fprintf(stderr, "pack_installer: failed reading '%s'\n", list.items[i].full_path);
            free(views);
            staged_list_free(&list);
            free(stub_bytes);
            return 1;
        }
        views[i].filename = list.items[i].rel_path;
        views[i].data     = list.items[i].data;
        views[i].size     = list.items[i].size;
        raw_total_bytes  += list.items[i].size;
    }

    uint8_t *zip_bytes = NULL;
    size_t   zip_len   = 0u;
    bool zip_ok = arcr_zip_create_mem(views, list.count, &zip_bytes, &zip_len);
    free(views);

    if (!zip_ok || !zip_bytes || zip_len == 0u) {
        fprintf(stderr, "pack_installer: failed creating PKZIP archive\n");
        free(zip_bytes);
        staged_list_free(&list);
        free(stub_bytes);
        return 1;
    }

    WsSfxTrailer trailer;
    memcpy(trailer.magic, WS_SFX_MAGIC_STR, WS_SFX_MAGIC_LEN);
    trailer.zip_offset = (uint64_t)stub_len;
    trailer.zip_size   = (uint64_t)zip_len;
    trailer.zip_crc32  = arcr_crc32(zip_bytes, zip_len);
    trailer.file_count = (uint32_t)list.count;

    uint8_t raw_trailer[WS_SFX_TRAILER_BYTES];
    ws_sfx_trailer_encode(&trailer, raw_trailer);

    FILE *out_fp = fopen(output_exe_path, "wb");
    if (!out_fp) {
        fprintf(stderr, "pack_installer: failed opening output '%s' for writing\n",
                output_exe_path);
        free(zip_bytes);
        staged_list_free(&list);
        free(stub_bytes);
        return 1;
    }

    bool wr_ok = (fwrite(stub_bytes, 1u, stub_len, out_fp) == stub_len) &&
                 (fwrite(zip_bytes, 1u, zip_len, out_fp) == zip_len) &&
                 (fwrite(raw_trailer, 1u, WS_SFX_TRAILER_BYTES, out_fp) == WS_SFX_TRAILER_BYTES);
    fclose(out_fp);
    free(stub_bytes);
    free(zip_bytes);

    if (!wr_ok) {
        fprintf(stderr, "pack_installer: short write on '%s'\n", output_exe_path);
        staged_list_free(&list);
        return 1;
    }

    if (!verify_packed_installer(output_exe_path, &list)) {
        staged_list_free(&list);
        return 1;
    }

    printf("Packed Waterscape SFX installer: '%s' (%lu files, %lu -> %lu ZIP bytes, CRC32=0x%08lX)\n",
           output_exe_path,
           (unsigned long)list.count,
           (unsigned long)raw_total_bytes,
           (unsigned long)zip_len,
           (unsigned long)trailer.zip_crc32);

    staged_list_free(&list);
    return 0;
}
