/* Actual Windows/POSIX file I/O, one image handle shared by independent readers. */
#include "../host/romfs_pack_file.h"
#include <stdio.h>
typedef struct { RomfsPackFile *file; unsigned id; int failed; } Worker;
static DWORD WINAPI worker(LPVOID context) {
    Worker *w = (Worker *)context;
    const RomfsPackEntry *entry = romfs_pack_find(&w->file->pack, "pattern");
    if (!entry || entry->size != 4096) { w->failed = 1; return 1; }
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned char bytes[37];
        size_t done = 0;
        uint64_t offset = (w->id * 97 + i * 53) % 4097;
        size_t want = 4096 - (size_t)offset < sizeof bytes ? 4096 - (size_t)offset : sizeof bytes;
        if (!romfs_pack_read(&w->file->pack, entry, offset, bytes, sizeof bytes, &done) || done != want) {
            w->failed = 1; return 2;
        }
        for (size_t j = 0; j < done; ++j)
            if (bytes[j] != (unsigned char)(((offset + j) * 17 + 3) % 251)) {
                w->failed = 1; return 3;
            }
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 90;
    RomfsPackFile file = {0};
    if (!romfs_pack_file_open(&file, argv[1])) return 2;
    if (argc == 3) { romfs_pack_file_close(&file); return 0; }
    Worker workers[8];
    HANDLE threads[8];
    unsigned started = 0;
    int result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        workers[i].file = &file;
        workers[i].id = i;
        workers[i].failed = 0;
        threads[i] = CreateThread(NULL, 0, worker, &workers[i], 0, NULL);
        if (!threads[i]) { result = 4; break; }
        ++started;
    }
    for (unsigned i = 0; i < started; ++i) {
        if (WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0 || workers[i].failed) result = 5;
        CloseHandle(threads[i]);
    }
    romfs_pack_file_close(&file);
    romfs_pack_file_close(&file);
    if (!result) puts("8000 concurrent positional reads passed");
    return result;
}
