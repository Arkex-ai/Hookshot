/* OS-backed positional I/O for romfs_pack.h. On Linux windows.h is Hookshot's POSIX layer.
   Include after the caller's UTF-8 path wrappers, as nn_fs.c does for all its file calls. */
#ifndef HOOKSHOT_ROMFS_PACK_FILE_H
#define HOOKSHOT_ROMFS_PACK_FILE_H
#include <windows.h>
#include "romfs_pack.h"
#ifdef _WIN32
typedef struct { wchar_t *name; int length; uint32_t entry; } RomfsPackWindowsName;
#endif

typedef struct {
    HANDLE handle;
    RomfsPack pack;
#ifdef _WIN32
    wchar_t *wide_names;
    RomfsPackWindowsName *names;
#endif
} RomfsPackFile;

#ifdef _WIN32
/* One bounded index at open, not a linear scan for every missing filename. No global sort context. */
static inline int romfs_pack_file_names(RomfsPackFile *file) {
    uint32_t count = file->pack.count;
    size_t units = 0;
    for (uint32_t i = 0; i < count; ++i) units += file->pack.entries[i].length + 1;
    file->wide_names = (wchar_t *)malloc((units ? units : 1) * sizeof(wchar_t));
    file->names = (RomfsPackWindowsName *)calloc(count ? count : 1, sizeof *file->names);
    RomfsPackWindowsName *scratch = (RomfsPackWindowsName *)calloc(count ? count : 1, sizeof *scratch);
    if (!file->wide_names || !file->names || !scratch) { free(scratch); return 0; }
    size_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const RomfsPackEntry *e = file->pack.entries + i;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    (const char *)file->pack.index + e->name, (int)e->length,
                                    file->wide_names + at, (int)e->length);
        if (!n) { free(scratch); return 0; }
        file->names[i].name = file->wide_names + at;
        file->names[i].length = n;
        file->names[i].entry = i;
        at += (size_t)n;
    }
    for (uint32_t width = 1; width < count; width *= 2) {
        for (uint32_t start = 0; start < count; start += 2 * width) {
            uint32_t middle = start + width < count ? start + width : count;
            uint32_t end = middle + width < count ? middle + width : count;
            uint32_t a = start, b = middle, out = start;
            while (a < middle && b < end) {
                int cmp = CompareStringOrdinal(file->names[a].name, file->names[a].length,
                                                file->names[b].name, file->names[b].length, TRUE);
                if (!cmp) { free(scratch); return 0; }
                scratch[out++] = cmp <= CSTR_EQUAL ? file->names[a++] : file->names[b++];
            }
            while (a < middle) scratch[out++] = file->names[a++];
            while (b < end) scratch[out++] = file->names[b++];
        }
        memcpy(file->names, scratch, (size_t)count * sizeof *scratch);
    }
    free(scratch);
    for (uint32_t i = 1; i < count; ++i) {
        int cmp = CompareStringOrdinal(file->names[i - 1].name, file->names[i - 1].length,
                                       file->names[i].name, file->names[i].length, TRUE);
        if (!cmp || cmp == CSTR_EQUAL) return 0;   /* ambiguous images cannot depend on record order */
    }
    return 1;
}
#endif

static inline const RomfsPackEntry *romfs_pack_file_find(const RomfsPackFile *file, const char *name) {
    const RomfsPackEntry *exact = romfs_pack_find(&file->pack, name);
    if (exact) return exact;
#ifdef _WIN32
    size_t bytes = strlen(name);
    if (!bytes || bytes > 0x300) return NULL;
    wchar_t wide[0x301];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)bytes, wide, 0x301);
    if (!n) return NULL;
    uint32_t lo = 0, hi = file->pack.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const RomfsPackWindowsName *e = file->names + mid;
        int cmp = CompareStringOrdinal(wide, n, e->name, e->length, TRUE);
        if (!cmp) return NULL;
        if (cmp == CSTR_EQUAL) return file->pack.entries + e->entry;
        if (cmp == CSTR_LESS_THAN) hi = mid;
        else lo = mid + 1;
    }
#endif
    return NULL;
}

static inline int romfs_pack_file_dir_open(const RomfsPackFile *file, const char *name, RomfsPackDir *out) {
    if (!*name) return romfs_pack_dir_open(&file->pack, name, out);
    const RomfsPackEntry *e = romfs_pack_file_find(file, name);
    if (!e || !e->directory) return 0;
    char canonical[0x301];
    memcpy(canonical, file->pack.index + e->name, e->length);
    canonical[e->length] = 0;
    return romfs_pack_dir_open(&file->pack, canonical, out);
}

static inline size_t romfs_pack_file_read(void *context, uint64_t offset, void *out, size_t size) {
    RomfsPackFile *file = (RomfsPackFile *)context;
    size_t done = 0;
    if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) return 0;
    while (done < size) {
        OVERLAPPED ov = {0};
        uint64_t at = offset + done;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD n = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), got = 0;
        BOOL ok = ReadFile(file->handle, (unsigned char *)out + done, n, &got, &ov);
        if (got > n) break;
        done += got;
        if (!ok || !got) break;
    }
    return done;
}

static inline void romfs_pack_file_close(RomfsPackFile *file) {
#ifdef _WIN32
    free(file->names); file->names = NULL;
    free(file->wide_names); file->wide_names = NULL;
#endif
    romfs_pack_close(&file->pack);
    if (file->handle && file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
    file->handle = INVALID_HANDLE_VALUE;
}

/* Startup only: out must not hold an existing file. A successful object must not move while in use. */
static inline int romfs_pack_file_open(RomfsPackFile *out, const char *path) {
    RomfsPackFile file = {0};
    LARGE_INTEGER before = {0}, after = {0};
    file.handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file.handle == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file.handle, &before) || before.QuadPart < 0 ||
        !romfs_pack_open(&file.pack, romfs_pack_file_read, &file, (uint64_t)before.QuadPart) ||
        !GetFileSizeEx(file.handle, &after) || before.QuadPart != after.QuadPart) {
        romfs_pack_file_close(&file);
        return 0;
    }
#ifdef _WIN32
    if (!romfs_pack_file_names(&file)) { romfs_pack_file_close(&file); return 0; }
#endif
    *out = file;
    out->pack.context = out;
    return 1;
}
#endif
