/* Optional RomFS image: bounded, immutable index and positional I/O supplied by the host.
   No game data, platform handles or shared file cursor are owned by this reader. */
#ifndef HOOKSHOT_ROMFS_PACK_H
#define HOOKSHOT_ROMFS_PACK_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef size_t (*RomfsPackRead)(void *context, uint64_t offset, void *out, size_t size);
typedef struct {
    uint32_t name, length, directory;
    uint64_t offset, size;
} RomfsPackEntry;
typedef struct {
    RomfsPackRead read;
    void *context;
    unsigned char *index;
    RomfsPackEntry *entries;
    uint32_t count;
} RomfsPack;

static inline uint32_t romfs_pack_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t romfs_pack_u64(const unsigned char *p) {
    return romfs_pack_u32(p) | (uint64_t)romfs_pack_u32(p + 4) << 32;
}
static inline uint32_t romfs_pack_crc(const unsigned char *p, size_t size) {
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static inline int romfs_pack_utf8(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++], count = 0, minimum = 0;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) count = 1, minimum = 0x80, c &= 31;
        else if (c >= 0xe0 && c <= 0xef) count = 2, minimum = 0x800, c &= 15;
        else if (c >= 0xf0 && c <= 0xf4) count = 3, minimum = 0x10000, c &= 7;
        else return 0;
        if (count > n - i) return 0;
        while (count--) {
            unsigned b = p[i++];
            if ((b & 0xc0) != 0x80) return 0;
            c = (c << 6) | (b & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
static inline int romfs_pack_name(const unsigned char *p, size_t n) {
    if (!n || n > 0x300 || !romfs_pack_utf8(p, n)) return 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && p[end] != '/') {
            if (p[end] < 32 || strchr("\\:*?\"<>|", p[end])) return 0;
            ++end;
        }
        size_t length = end - at;
        if (!length || p[end - 1] == ' ' || p[end - 1] == '.') return 0;
        /* Windows device names remain special with an extension or spaces before it. */
        unsigned char stem[16];
        size_t k = 0;
        while (k < length && k < sizeof stem - 1 && p[at + k] != '.') {
            unsigned char c = p[at + k];
            stem[k++] = c >= 'a' && c <= 'z' ? (unsigned char)(c - 32) : c;
        }
        if (k == length || p[at + k] == '.') {
            while (k && stem[k - 1] == ' ') --k;
            stem[k] = 0;
            if (!strcmp((char *)stem, "CON") || !strcmp((char *)stem, "PRN") ||
                !strcmp((char *)stem, "AUX") || !strcmp((char *)stem, "NUL") ||
                !strcmp((char *)stem, "CONIN$") || !strcmp((char *)stem, "CONOUT$") ||
                !strcmp((char *)stem, "CLOCK$")) return 0;
            if (k >= 4 && (!memcmp(stem, "COM", 3) || !memcmp(stem, "LPT", 3))) {
                if (k == 4 && stem[3] >= '0' && stem[3] <= '9') return 0;
                if (k == 5 && stem[3] == 0xc2 &&
                    (stem[4] == 0xb9 || stem[4] == 0xb2 || stem[4] == 0xb3)) return 0;
            }
        }
        if (end == n) break;
        at = end + 1;
        if (at == n) return 0;
    }
    return 1;
}
static inline int romfs_pack_compare(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    int cmp = memcmp(a, b, an < bn ? an : bn);
    return cmp ? cmp : (an > bn) - (an < bn);
}
static inline uint32_t romfs_pack_lower(const RomfsPack *p, const unsigned char *name, size_t n) {
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const RomfsPackEntry *e = p->entries + mid;
        int cmp = romfs_pack_compare(name, n, p->index + e->name, e->length);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return mid;
    }
    return lo;
}
static inline const RomfsPackEntry *romfs_pack_find_n(const RomfsPack *p, const unsigned char *name, size_t n) {
    uint32_t at = romfs_pack_lower(p, name, n);
    if (at < p->count) {
        const RomfsPackEntry *e = p->entries + at;
        if (!romfs_pack_compare(name, n, p->index + e->name, e->length)) return e;
    }
    return NULL;
}
static inline const RomfsPackEntry *romfs_pack_find(const RomfsPack *p, const char *name) {
    return romfs_pack_find_n(p, (const unsigned char *)name, strlen(name));
}
static inline void romfs_pack_close(RomfsPack *p) {
    free(p->entries);
    free(p->index);
    memset(p, 0, sizeof *p);
}
/* Caller closes its previous pack first. A failed open leaves *out unchanged.
   Callback returns bytes actually written, at most size; it must support concurrent callers. */
static inline int romfs_pack_open(RomfsPack *out, RomfsPackRead read, void *context, uint64_t bytes) {
    unsigned char h[64];
    RomfsPack p = {0};
    uint64_t index_at, index_size, next = 64;
    uint32_t count, header_crc;
    size_t cursor = 0;
    if (!read || bytes < 64 || bytes > INT64_MAX || read(context, 0, h, sizeof h) != sizeof h) return 0;
    if (memcmp(h, "HROMFS1\0", 8) || romfs_pack_u32(h + 12) || romfs_pack_u64(h + 56)) return 0;
    header_crc = romfs_pack_u32(h + 52);
    memset(h + 52, 0, 4);
    if (romfs_pack_crc(h, sizeof h) != header_crc) return 0;
    count = romfs_pack_u32(h + 8);
    index_at = romfs_pack_u64(h + 16);
    index_size = romfs_pack_u64(h + 24);
    if (index_at < 64 || index_at > bytes || index_size != bytes - index_at ||
        romfs_pack_u64(h + 32) != index_at || romfs_pack_u64(h + 40) != bytes ||
        count > 1000000u || index_size > (64u << 20) || count > index_size / 25) return 0;
    p.read = read;
    p.context = context;
    p.index = (unsigned char *)malloc(index_size ? (size_t)index_size : 1);
    p.entries = (RomfsPackEntry *)calloc(count ? count : 1, sizeof *p.entries);
    if (!p.index || !p.entries) goto bad;
    if (index_size && read(context, index_at, p.index, (size_t)index_size) != index_size) goto bad;
    if (romfs_pack_crc(p.index, (size_t)index_size) != romfs_pack_u32(h + 48)) goto bad;
    for (uint32_t i = 0; i < count; ++i) {
        RomfsPackEntry *e = p.entries + i;
        if (index_size - cursor < 24) goto bad;
        const unsigned char *r = p.index + cursor;
        e->length = romfs_pack_u32(r);
        e->directory = romfs_pack_u32(r + 4);
        e->offset = romfs_pack_u64(r + 8);
        e->size = romfs_pack_u64(r + 16);
        cursor += 24;
        e->name = (uint32_t)cursor;
        if (e->length > index_size - cursor || e->directory > 1 ||
            !romfs_pack_name(p.index + cursor, e->length)) goto bad;
        if (i) {
            const RomfsPackEntry *prev = e - 1;
            if (romfs_pack_compare(p.index + prev->name, prev->length,
                                   p.index + e->name, e->length) >= 0) goto bad;
        }
        /* Every non-root parent must already exist and be a directory. */
        for (size_t k = e->length; k; --k) {
            if (p.index[cursor + k - 1] != '/') continue;
            const RomfsPackEntry *parent = romfs_pack_find_n(&p, p.index + cursor, k - 1);
            if (!parent || !parent->directory) goto bad;
            break;
        }
        if (e->directory) {
            if (e->offset || e->size) goto bad;
        } else {
            if (e->offset != next || next > index_at || e->size > index_at - next) goto bad;
            next += e->size;
        }
        cursor += e->length;
        p.count = i + 1;
    }
    if (cursor != index_size || next != index_at) goto bad;
    *out = p;
    return 1;
bad:
    romfs_pack_close(&p);
    return 0;
}
/* True with zero bytes at EOF; false on invalid range/type or an I/O failure.
   Entries and the callback's backing file must remain alive throughout all reads. */
static inline int romfs_pack_read(const RomfsPack *p, const RomfsPackEntry *e, uint64_t offset,
                                  void *out, size_t size, size_t *done) {
    *done = 0;
    if (!e || e->directory || offset > e->size) return 0;
    if (e->size - offset < size) size = (size_t)(e->size - offset);
    if (!size) return 1;
    size_t got = p->read(p->context, e->offset + offset, out, size);
    if (got > size) return 0;
    *done = got;
    return got == size;
}

typedef struct {
    uint32_t cursor;
    size_t prefix_length;
    unsigned char prefix[0x302];
} RomfsPackDir;

/* Directory names are relative, normalized UTF-8; the empty name opens the root. */
static inline int romfs_pack_dir_open(const RomfsPack *p, const char *name, RomfsPackDir *out) {
    size_t n = strlen(name);
    if (n > 0x300) return 0;
    if (n) {
        const RomfsPackEntry *e = romfs_pack_find_n(p, (const unsigned char *)name, n);
        if (!e || !e->directory) return 0;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->prefix, name, n);
    if (n) out->prefix[n++] = '/';
    out->prefix_length = n;
    out->cursor = romfs_pack_lower(p, out->prefix, n);
    return 1;
}

/* Mode bits:1 directories,2 files. Returns immediate children only, with a length-delimited basename.
   Each open directory has its own cursor; the immutable pack may be shared. */
static inline const RomfsPackEntry *romfs_pack_dir_next(const RomfsPack *p, RomfsPackDir *dir, unsigned mode,
                                                      const unsigned char **name, size_t *length) {
    while (dir->cursor < p->count) {
        const RomfsPackEntry *e = p->entries + dir->cursor++;
        const unsigned char *full = p->index + e->name;
        if (e->length < dir->prefix_length || memcmp(full, dir->prefix, dir->prefix_length)) {
            dir->cursor = p->count;
            break;
        }
        const unsigned char *base = full + dir->prefix_length;
        size_t n = e->length - dir->prefix_length;
        const unsigned char *slash = (const unsigned char *)memchr(base, '/', n);
        if (slash) {
            /* Skip this child's subtree by lower bound, without skipping sibling names such as dir.ext. */
            unsigned char after[0x302];
            size_t k = (size_t)(slash - full) + 1;
            memcpy(after, full, k);
            after[k - 1] = '0';   /* '/' + 1, an exclusive upper bound on this prefix */
            dir->cursor = romfs_pack_lower(p, after, k);
            continue;
        }
        if (!(mode & (e->directory ? 1u : 2u))) continue;
        *name = base;
        *length = n;
        return e;
    }
    return NULL;
}

/* Total selected entries, independent of how far the original cursor has been read. */
static inline uint32_t romfs_pack_dir_count(const RomfsPack *p, const RomfsPackDir *dir, unsigned mode) {
    RomfsPackDir copy = *dir;
    const unsigned char *name;
    size_t length;
    uint32_t count = 0;
    copy.cursor = romfs_pack_lower(p, copy.prefix, copy.prefix_length);
    while (romfs_pack_dir_next(p, &copy, mode, &name, &length)) ++count;
    return count;
}
#endif
