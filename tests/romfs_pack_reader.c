/* Authored pack reader harness; never reads a game or uses Vulkan. */
#include "../host/romfs_pack.h"
#include <stdio.h>

typedef struct { const unsigned char *data; size_t size; int fail; } Input;
static size_t read_at(void *context, uint64_t offset, void *out, size_t size) {
    Input *in = (Input *)context;
    if (in->fail == 1 || offset > in->size || size > in->size - offset) return 0;
    if ((in->fail == 2 || (in->fail == 3 && offset)) && size > 2) size = 2;
    memcpy(out, in->data + (size_t)offset, size);
    return size;
}
static int decode_name(const char *hex, char name[0x301]) {
    size_t n = strlen(hex);
    if ((n & 1) || n / 2 >= 0x301) return 0;
    for (size_t i = 0; i < n; i += 2) {
        unsigned byte;
        if (sscanf(hex + i, "%2x", &byte) != 1) return 0;
        name[i / 2] = (char)byte;
    }
    name[n / 2] = 0;
    return 1;
}
int main(int argc, char **argv) {
    if (argc < 3) return 90;
    FILE *file = fopen(argv[2], "rb");
    if (!file) return 91;
    if (fseek(file, 0, SEEK_END)) return 92;
    long length = ftell(file);
    if (length < 0 || length > (128 << 20) || fseek(file, 0, SEEK_SET)) return 93;
    unsigned char *data = (unsigned char *)malloc(length ? (size_t)length : 1);
    if (!data) return 94;
    if (fread(data, 1, (size_t)length, file) != (size_t)length) return 95;
    fclose(file);
    Input input = {data, (size_t)length, 0};
    if (!strcmp(argv[1], "short-open")) input.fail = 2;
    if (!strcmp(argv[1], "short-index")) input.fail = 3;
    RomfsPack pack = {0};
    if (!romfs_pack_open(&pack, read_at, &input, (uint64_t)length)) { free(data); return 2; }
    /* A failed second open must leave the still-owned image usable. */
    RomfsPack previous = pack;
    input.fail = 1;
    if (romfs_pack_open(&pack, read_at, &input, (uint64_t)length) ||
        pack.index != previous.index || pack.entries != previous.entries ||
        pack.count != previous.count || pack.read != previous.read || pack.context != previous.context) return 88;
    input.fail = 0;
    int result = 0;
    if (!strcmp(argv[1], "check")) printf("%u\n", pack.count);
    else if (!strcmp(argv[1], "list") && argc == 6) {
        char path[0x301];
        RomfsPackDir dir;
        if (!decode_name(argv[3], path)) return 96;
        unsigned mode = (unsigned)strtoul(argv[4], NULL, 10);
        unsigned page = (unsigned)strtoul(argv[5], NULL, 10);
        if (!page) return 97;
        if (!romfs_pack_dir_open(&pack, path, &dir)) result = 3;
        else {
            uint32_t total = romfs_pack_dir_count(&pack, &dir, mode), completed = 0;
            printf("count=%u\n", total);
            do {
                for (unsigned i = 0; i < page; ++i) {
                    const unsigned char *name;
                    size_t n;
                    const RomfsPackEntry *entry = romfs_pack_dir_next(&pack, &dir, mode, &name, &n);
                    if (!entry) break;
                    printf("%c:", entry->directory ? 'D' : 'F');
                    for (size_t k = 0; k < n; ++k) printf("%02x", name[k]);
                    printf(":%llu\n", (unsigned long long)entry->size);
                    ++completed;
                }
                uint32_t before = dir.cursor;
                if (romfs_pack_dir_count(&pack, &dir, mode) != total || dir.cursor != before) return 98;
            } while (completed < total);
            const unsigned char *name;
            size_t n;
            if (romfs_pack_dir_next(&pack, &dir, mode, &name, &n)) return 99;
        }
    } else if (argc == 6 || argc == 7) {
        char name[0x301];
        if (!decode_name(argv[3], name)) return 96;
        const RomfsPackEntry *entry = romfs_pack_find(&pack, name);
        uint64_t offset = strtoull(argv[4], NULL, 10);
        size_t size = (size_t)strtoull(argv[5], NULL, 10), done = 0;
        unsigned char buffer[4096];
        if (size > sizeof buffer) return 98;
        input.fail = argc == 7 ? (!strcmp(argv[6], "partial") ? 2 : 1) : 0;
        if (!romfs_pack_read(&pack, entry, offset, buffer, size, &done)) {
            if (input.fail == 2) {
                printf("%zu:", done);
                for (size_t i = 0; i < done; ++i) printf("%02x", buffer[i]);
                puts("");
            }
            result = 3;
        }
        else {
            for (size_t i = 0; i < done; ++i) printf("%02x", buffer[i]);
            puts("");
        }
    } else result = 99;
    romfs_pack_close(&pack);
    if (pack.index || pack.entries || pack.read || pack.context || pack.count) return 89;
    romfs_pack_close(&pack);
    free(data);
    return result;
}
