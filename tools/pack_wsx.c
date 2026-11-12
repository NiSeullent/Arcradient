/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * tools/pack_wsx.c - Pure C11 `.wsx` WebExtension PKZIP Packager CLI
 *
 * Usage:
 *   pack_wsx <input_addon_dir> <output_file.wsx>
 *
 * Scans `<input_addon_dir>` (cross-platform Win32 FindFirstFileA / POSIX opendir),
 * packages `manifest.json` + present extension assets (`filter_rules.txt`, `inject.css`,
 * `content.js`, and any extra top-level asset files) into a DEFLATE-compressed `.wsx`
 * PKZIP archive via `arcr_zip_create_mem`, verifies `manifest.json` round-trip extraction
 * via `arcr_zip_extract_entry`, and atomically writes `<output_file.wsx>`.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"

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
#  include <unistd.h>
#endif

#define WSX_MAX_PACK_FILES 32u

typedef struct LoadedPackFile {
    char     name[128];
    uint8_t *data;
    size_t   size;
} LoadedPackFile;

static void join_path(char *out, size_t cap, const char *dir, const char *file) {
    size_t dlen = strlen(dir);
    while (dlen > 0u && (dir[dlen - 1u] == '/' || dir[dlen - 1u] == '\\')) --dlen;
#if defined(_WIN32)
    snprintf(out, cap, "%.*s\\%s", (int)dlen, dir, file);
#else
    snprintf(out, cap, "%.*s/%s", (int)dlen, dir, file);
#endif
}

static bool load_regular_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    if (!path || !out_buf || !out_size) return false;
    *out_buf  = NULL;
    *out_size = 0u;

#if !defined(_WIN32)
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
#endif

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long len = ftell(fp);
    if (len < 0L || len > 32L * 1024L * 1024L) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    size_t ulen = (size_t)len;
    uint8_t *buf = (uint8_t *)malloc(ulen + 1u);
    if (!buf) {
        fclose(fp);
        return false;
    }
    if (ulen > 0u && fread(buf, 1u, ulen, fp) != ulen) {
        free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    buf[ulen] = 0u;
    *out_buf  = buf;
    *out_size = ulen;
    return true;
}

static bool has_entry_name(const LoadedPackFile *files, size_t count, const char *name) {
    for (size_t i = 0u; i < count; ++i) {
        if (strcmp(files[i].name, name) == 0) return true;
    }
    return false;
}

static bool try_add_file(LoadedPackFile *files, size_t *inout_count,
                         const char *input_dir, const char *filename) {
    if (*inout_count >= WSX_MAX_PACK_FILES) return false;
    if (!filename || filename[0] == '\0' || filename[0] == '.' ||
        strstr(filename, "..") != NULL) {
        return false;
    }
    if (has_entry_name(files, *inout_count, filename)) return true;

    char full_path[1024];
    join_path(full_path, sizeof(full_path), input_dir, filename);

    uint8_t *data = NULL;
    size_t   size = 0u;
    if (!load_regular_file(full_path, &data, &size)) return false;

    size_t idx = (*inout_count)++;
    arcr_str_copy(files[idx].name, sizeof(files[idx].name), filename);
    files[idx].data = data;
    files[idx].size = size;
    return true;
}

static size_t discover_extra_files(LoadedPackFile *files, size_t *inout_count, const char *input_dir) {
    size_t added = 0u;
#if defined(_WIN32)
    char pattern[1024];
    join_path(pattern, sizeof(pattern), input_dir, "*");
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0u;
    do {
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            ffd.cFileName[0] != '.' &&
            !has_entry_name(files, *inout_count, ffd.cFileName)) {
            if (try_add_file(files, inout_count, input_dir, ffd.cFileName)) {
                ++added;
            }
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    DIR *dp = opendir(input_dir);
    if (!dp) return 0u;
    struct dirent *de = NULL;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] != '.' && !has_entry_name(files, *inout_count, de->d_name)) {
            if (try_add_file(files, inout_count, input_dir, de->d_name)) {
                ++added;
            }
        }
    }
    closedir(dp);
#endif
    return added;
}

static bool write_file_atomic(const char *out_path, const uint8_t *bytes, size_t len) {
    char tmp_path[1024];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu",
                     out_path, (unsigned long)len);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) return false;

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return false;
    bool write_ok = (len == 0u) || (fwrite(bytes, 1u, len, fp) == len);
    if (fflush(fp) != 0) write_ok = false;
    if (fclose(fp) != 0) write_ok = false;

    if (!write_ok) {
        remove(tmp_path);
        return false;
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path, out_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileA(out_path);
        if (!MoveFileA(tmp_path, out_path)) {
            (void)DeleteFileA(tmp_path);
            return false;
        }
    }
    return true;
#else
    if (rename(tmp_path, out_path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
#endif
}

static void free_loaded_files(LoadedPackFile *files, size_t count) {
    for (size_t i = 0u; i < count; ++i) {
        free(files[i].data);
        files[i].data = NULL;
        files[i].size = 0u;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_addon_dir> <output_file.wsx>\n",
                (argc > 0 && argv[0]) ? argv[0] : "pack_wsx");
        return 1;
    }

    const char *input_dir = argv[1];
    const char *out_wsx   = argv[2];

    LoadedPackFile files[WSX_MAX_PACK_FILES];
    memset(files, 0, sizeof(files));
    size_t file_count = 0u;

    /* 1. Require `manifest.json` as entry #0 and validate minimal manifest keys */
    if (!try_add_file(files, &file_count, input_dir, "manifest.json") || files[0].size == 0u) {
        fprintf(stderr, "pack_wsx error: missing or empty manifest.json in '%s'\n", input_dir);
        free_loaded_files(files, file_count);
        return 1;
    }
    if (strstr((const char *)files[0].data, "\"id\"") == NULL ||
        strstr((const char *)files[0].data, "\"name\"") == NULL) {
        fprintf(stderr, "pack_wsx error: manifest.json in '%s' must define \"id\" and \"name\"\n",
                input_dir);
        free_loaded_files(files, file_count);
        return 1;
    }

    /* 2. Collect canonical WebExtension payload files + any extra regular files in `<input_addon_dir>` */
    (void)try_add_file(files, &file_count, input_dir, "filter_rules.txt");
    (void)try_add_file(files, &file_count, input_dir, "inject.css");
    (void)try_add_file(files, &file_count, input_dir, "content.js");
    (void)discover_extra_files(files, &file_count, input_dir);

    ArcrZipEntryView views[WSX_MAX_PACK_FILES];
    for (size_t i = 0u; i < file_count; ++i) {
        views[i].filename = files[i].name;
        views[i].data     = files[i].data;
        views[i].size     = files[i].size;
    }

    /* 3. Build PKZIP `.wsx` archive in memory */
    uint8_t *zip_bytes = NULL;
    size_t   zip_len   = 0u;
    if (!arcr_zip_create_mem(views, file_count, &zip_bytes, &zip_len) || !zip_bytes) {
        fprintf(stderr, "pack_wsx error: arcr_zip_create_mem failed for '%s'\n", input_dir);
        free_loaded_files(files, file_count);
        return 1;
    }

    /* 4. Round-trip verify `manifest.json` extraction & byte-exact integrity */
    uint8_t *rt_manifest = NULL;
    size_t   rt_len      = 0u;
    if (!arcr_zip_extract_entry(zip_bytes, zip_len, "manifest.json", &rt_manifest, &rt_len) ||
        rt_len != files[0].size ||
        memcmp(rt_manifest, files[0].data, rt_len) != 0) {
        fprintf(stderr, "pack_wsx error: round-trip verification of manifest.json failed\n");
        free(rt_manifest);
        free(zip_bytes);
        free_loaded_files(files, file_count);
        return 1;
    }
    free(rt_manifest);

    /* 5. Atomically write `<output_file.wsx>` */
    if (!write_file_atomic(out_wsx, zip_bytes, zip_len)) {
        fprintf(stderr, "pack_wsx error: failed to write '%s'\n", out_wsx);
        free(zip_bytes);
        free_loaded_files(files, file_count);
        return 1;
    }

    printf("Packed %lu file(s) from '%s' -> '%s' (%lu bytes)\n",
           (unsigned long)file_count, input_dir, out_wsx, (unsigned long)zip_len);

    free(zip_bytes);
    free_loaded_files(files, file_count);
    return 0;
}
