/* Authored guest memory; the complete nn_fs.c is compiled in, not a replacement file-system model. */
#include "authored_hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
unsigned char authored_guest[0x10000];
int hos_trace;
static int held = 1, dirty_calls;
static size_t dirty_size;
static int use_mod, use_wide, blank_icon, use_directory_mods;
static const char *mod_file, *wide_file;
void hos_log(const char *format, ...) { (void)format; }
void hos_fatal(Ctx *c, const char *format, ...) { (void)c; (void)format; abort(); }
int hos_vcore_away(void) { int was = held; held = 0; return was; }
void hos_vcore_back(void) { held = 1; }
int64_t hos_now_ns(void) { return 0; }
void hos_sleep_ns(int64_t n) { (void)n; }
void authored_written(void *p, size_t n) { (void)p; ++dirty_calls; dirty_size = n; }
int hos_mods_romfs(const char *rel, char *out, size_t cap) {
    if (!use_mod || strcmp(rel, "pattern")) return 0;
    if (held) abort();
    snprintf(out, cap, "%s", mod_file);
    return 1;
}
int hos_mods_dir_extras(const char *rel, const char *base,
                       void (*fn)(void *, const char *, int, uint64_t), void *arg) {
    (void)base;
    if (!use_directory_mods) return 0;
    if (!*rel) {
#ifdef _WIN32
        fn(arg, "PATTERN", 0, 3);   /* a Windows case alias must not become a duplicate */
#else
        fn(arg, "pattern", 0, 3);
#endif
        fn(arg, "new-dir", 1, 0); fn(arg, "new-file", 0, 3);
        return 3;
    }
    if (!strcmp(rel, "new-dir")) { fn(arg, "child", 0, 5); return 1; }
    if (!strcmp(rel, "nested")) { fn(arg, "one", 0, 99); return 1; }
    return 0;
}
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) {
    if (!use_wide || strcmp(guest, "rom:/pattern")) return 0;
    snprintf(out, cap, "%s", wide_file);
    return 1;
}
int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest; (void)host; return blank_icon; }
#include "nn_fs.c"

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); return 1; } } while (0)
static uint64_t open_guest(const char *path, int mode, uint64_t *result) {
    strcpy((char *)GP(0x1000), path);
    Ctx c = {{0}};
    c.x[0] = 0x1100; c.x[1] = 0x1000; c.x[2] = (uint64_t)mode;
    held = 1;
    imp__ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci(&c);
    *result = c.x[0];
    return G64(0x1100);
}
static void close_guest(uint64_t h) {
    Ctx c = {{0}}; c.x[0] = h;
    imp__ZN2nn2fs9CloseFileENS0_10FileHandleE(&c);
}
static uint64_t size_guest(uint64_t h) {
    Ctx c = {{0}}; c.x[0] = 0x1200; c.x[1] = h;
    imp__ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE(&c);
    return G64(0x1200);
}
static uint64_t read_guest(uint64_t h, uint64_t offset, uint64_t buffer, uint64_t size) {
    Ctx c = {{0}};
    c.x[0] = 0x1300; c.x[1] = h; c.x[2] = offset; c.x[3] = buffer; c.x[4] = size;
    dirty_calls = 0; dirty_size = 0; held = 1;
    imp__ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm(&c);
    if (c.x[0]) abort();
    return G64(0x1300);
}
static uint64_t type_guest(const char *path, uint64_t *result) {
    strcpy((char *)GP(0x1000), path);
    Ctx c = {{0}}; c.x[0] = 0x1400; c.x[1] = 0x1000;
    imp__ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc(&c);
    *result = c.x[0]; return G32(0x1400);
}
static uint64_t open_directory(const char *path, int mode, uint64_t *result) {
    strcpy((char *)GP(0x1000), path);
    Ctx c = {{0}}; c.x[0] = 0x1100; c.x[1] = 0x1000; c.x[2] = (uint64_t)mode;
    imp__ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci(&c);
    *result = c.x[0]; return G64(0x1100);
}
static uint64_t count_directory(uint64_t h) {
    Ctx c = {{0}}; c.x[0] = 0x1500; c.x[1] = h;
    imp__ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE(&c);
    if (c.x[0]) abort();
    return G64(0x1500);
}
static uint64_t read_directory(uint64_t h, uint64_t address, uint64_t max, uint64_t *result) {
    Ctx c = {{0}}; c.x[0] = 0x1600; c.x[1] = address; c.x[2] = h; c.x[3] = max;
    imp__ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl(&c);
    *result = c.x[0]; return G64(0x1600);
}
static void close_directory(uint64_t h) {
    Ctx c = {{0}}; c.x[0] = h;
    imp__ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE(&c);
}
static void set_env(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1); else unsetenv(key);
#endif
}
static int test_main(int argc, char **argv) {
    if (argc != 6) return 90;
    set_env("HOS_FS_DELAY_MS", NULL); set_env("HOS_FS_WATCH", NULL);
    set_env("HOS_FS_OVERLAY", NULL); set_env("HOS_NO_MOVIES", NULL);
    hos_romfs_dir = argv[2]; mod_file = argv[3]; wide_file = argv[4];
    uint64_t asset_size = 0;
    unsigned char asset[37];
    CHECK(hos_fs_romfs_read("pattern", 0, NULL, 0, &asset_size) && asset_size == 5 && held == 1);
    CHECK(hos_fs_romfs_read("pattern", 0, asset, 5, &asset_size) && !memcmp(asset, "LOOSE", 5));
    CHECK(!hos_fs_romfs_read("../pattern", 0, NULL, 0, &asset_size));
    CHECK(mount("rom", hos_romfs_dir, 0));
    CHECK(mount("save", hos_romfs_dir, 1));
    CHECK(hos_fs_pack_open(argv[1]));
    CHECK(!hos_fs_pack_open(argv[1]));
    dirty_calls = 0;
    CHECK(hos_fs_romfs_read("pattern", 0, NULL, 0, &asset_size) && asset_size == 4096 && held == 1);
    CHECK(hos_fs_romfs_read("pattern", 17, asset, sizeof asset, &asset_size) && !dirty_calls);
    for (size_t i = 0; i < sizeof asset; ++i) CHECK(asset[i] == (unsigned char)(((17 + i) * 17 + 3) % 251));
    CHECK(!hos_fs_romfs_read("pattern", 4090, asset, sizeof asset, &asset_size));
    CHECK(!hos_fs_romfs_read("Movie", 0, NULL, 0, &asset_size));
    CHECK(!hos_fs_romfs_read("loose-only", 0, NULL, 0, &asset_size));
    uint64_t result = 0;
    uint64_t h = open_guest("rom:/pattern", 1, &result);
    CHECK(result == 0 && held == 1);
    CHECK(size_guest(h) == 4096);
    CHECK(read_guest(h, 17, 0x2000, 37) == 37 && held == 1 && dirty_calls == 1 && dirty_size == 37);
    for (size_t i = 0; i < 37; ++i) CHECK(authored_guest[0x2000 + i] == (unsigned char)(((17 + i) * 17 + 3) % 251));
    CHECK(read_guest(h, 4090, 0x2000, 37) == 6 && dirty_size == 6);
    CHECK(read_guest(h, 4096, 0x2000, 37) == 0 && dirty_size == 0);
    CHECK(read_guest(h, 0, 0xfffc, 8) == 0 && !dirty_calls);
    CHECK(read_guest(h, UINT64_MAX, 0x2000, 8) == 0 && dirty_size == 0);
    Ctx c = {{0}}; c.x[0] = h; c.x[1] = 4090; c.x[2] = 0x2000; c.x[3] = 37;
    imp__ZN2nn2fs8ReadFileENS0_10FileHandleElPvm(&c);
    CHECK(c.x[0] == 0x2EE202 && dirty_size == 6);
    close_guest(h);
    h = open_guest("rom:/pattern", 1, &result);
    CHECK(result == 0 && size_guest(h) == 4096); close_guest(h);
    open_guest("rom:/loose-only", 1, &result);
    CHECK(result == R_PATH_NOT_FOUND);
    open_guest("rom:/pattern", 2, &result);
    CHECK(result == R_PATH_NOT_FOUND);
    h = open_guest("save:/pattern", 1, &result);
    CHECK(result == 0 && size_guest(h) == 5);
    CHECK(read_guest(h, 0, 0x2000, 20) == 5 && !memcmp(GP(0x2000), "LOOSE", 5)); close_guest(h);
    use_mod = 1;
    h = open_guest("rom:/pattern", 1, &result);
    CHECK(result == 0 && read_guest(h, 0, 0x2000, 20) == 3 && !memcmp(GP(0x2000), "MOD", 3)); close_guest(h);
    use_wide = 1;
    h = open_guest("rom:/pattern", 1, &result);
    CHECK(result == 0 && read_guest(h, 0, 0x2000, 20) == 4 && !memcmp(GP(0x2000), "WIDE", 4)); close_guest(h);
    set_env("HOS_FS_OVERLAY", argv[5]);
    h = open_guest("rom:/pattern", 1, &result);
    CHECK(result == 0 && read_guest(h, 0, 0x2000, 20) == 7 && !memcmp(GP(0x2000), "OVERLAY", 7)); close_guest(h);
    set_env("HOS_FS_OVERLAY", NULL); use_mod = use_wide = 0;
    blank_icon = 1; open_guest("rom:/pattern", 1, &result);
    CHECK(result == R_PATH_NOT_FOUND); blank_icon = 0;
    h = open_guest("rom:/Movie/a.webm", 1, &result);
    CHECK(result == 0); close_guest(h);
    set_env("HOS_NO_MOVIES", "1"); open_guest("rom:/Movie/a.webm", 1, &result);
    CHECK(result == R_PATH_NOT_FOUND && held == 1);
    set_env("HOS_NO_MOVIES", NULL);
    CHECK(type_guest("rom:/", &result) == 0 && result == 0);
    CHECK(type_guest("rom:/pattern", &result) == 1 && result == 0);
    CHECK(type_guest("rom:/empty-dir", &result) == 0 && result == 0);
    type_guest("rom:/loose-only", &result); CHECK(result == R_PATH_NOT_FOUND);
    blank_icon = 1; type_guest("rom:/pattern", &result); CHECK(result == R_PATH_NOT_FOUND); blank_icon = 0;
    h = open_directory("rom:/empty-dir", 3, &result);
    CHECK(result == 0 && count_directory(h) == 0);
    CHECK(read_directory(h, 0x2000, 1, &result) == 0 && result == 0);
    close_directory(h);
    open_directory("rom:/absent", 3, &result); CHECK(result == R_PATH_NOT_FOUND);
    open_directory("rom:/pattern", 3, &result); CHECK(result == R_PATH_NOT_FOUND);
    use_directory_mods = 1;
    CHECK(type_guest("rom:/new-dir", &result) == 0 && result == 0);
    h = open_directory("rom:/new-dir", 3, &result);
    CHECK(result == 0 && count_directory(h) == 1);
    CHECK(read_directory(h, 0x2000, 1, &result) == 1 && result == 0);
    CHECK(!strcmp((char *)GP(0x2000), "child") && G64(0x2308) == 5);
    CHECK(count_directory(h) == 1); close_directory(h);
    h = open_directory("rom:/", 3, &result);
    CHECK(result == 0 && count_directory(h) == 6);
    CHECK(read_directory(h, 0xfffc, 1, &result) == 0 && result == 0x2EE202);
    CHECK(TryAcquireSRWLockExclusive(&dir_lock));
    ReleaseSRWLockExclusive(&dir_lock);
    CHECK(read_directory(h, 0x2000, UINT64_MAX, &result) == 0 && result == 0x2EE202);
    CHECK(read_directory(h, UINT64_MAX, 0, &result) == 0 && result == 0);
    CHECK(count_directory(h) == 6);
    const char *names[] = {"Movie", "empty-dir", "nested", "pattern", "new-dir", "new-file"};
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(read_directory(h, 0x2000, 1, &result) == 1 && result == 0);
        CHECK(!strcmp((char *)GP(0x2000), names[i]));
        CHECK(count_directory(h) == 6);
    }
    CHECK(read_directory(h, 0x2000, 1, &result) == 0 && result == 0); close_directory(h);
    h = open_directory("rom:/", 2, &result);
    CHECK(result == 0 && count_directory(h) == 2);
    CHECK(read_directory(h, 0x2000, 4, &result) == 2 && result == 0);
    CHECK(!strcmp((char *)GP(0x2000), "pattern") && G64(0x2308) == 4096);
    CHECK(!strcmp((char *)GP(0x2310), "new-file") && G64(0x2618) == 3); close_directory(h);
    h = open_directory("rom:/", 1, &result);
    CHECK(result == 0 && count_directory(h) == 4);
    CHECK(read_directory(h, 0x2000, 4, &result) == 4 && result == 0);
    CHECK(!strcmp((char *)GP(0x2930), "new-dir")); close_directory(h);
    h = open_directory("rom:/nested", 3, &result);
    CHECK(result == 0 && count_directory(h) == 2);
    CHECK(read_directory(h, 0x2000, 4, &result) == 2 && result == 0);
    CHECK(!strcmp((char *)GP(0x2000), "deeper"));
    CHECK(!strcmp((char *)GP(0x2310), "one") && G64(0x2618) == 3); close_directory(h);
#ifdef _WIN32
    char baseline[HOS_FS_PATH];
    snprintf(baseline, sizeof baseline, "%s\\mOvIe\\CAF\xc3\x89", hos_romfs_dir);
    HANDLE loose = CreateFileA(baseline, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(loose != INVALID_HANDLE_VALUE); CloseHandle(loose);
    h = open_guest("rom:/mOvIe/CAF\xc3\x89", 1, &result);
    CHECK(result == 0 && read_guest(h, 0, 0x2000, 20) == 7 && !memcmp(GP(0x2000), "UNICODE", 7)); close_guest(h);
    CHECK(type_guest("rom:/MOVIE/mIxEd", &result) == 0 && result == 0);
    snprintf(baseline, sizeof baseline, "%s\\MOVIE\\mIxEd", hos_romfs_dir);
    CHECK(GetFileAttributesA(baseline) & FILE_ATTRIBUTE_DIRECTORY);
    h = open_directory("rom:/MOVIE/mIxEd", 3, &result);
    CHECK(result == 0 && count_directory(h) == 1);
    CHECK(read_directory(h, 0x2000, 2, &result) == 1 && result == 0);
    CHECK(!strcmp((char *)GP(0x2000), "Leaf")); close_directory(h);
#else
    open_guest("rom:/mOvIe/CAF\xc3\x89", 1, &result);
    CHECK(result == R_PATH_NOT_FOUND);
#endif
    printf("%u full nn_fs packed-handle checks passed\n", checks);
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char **args = (char **)calloc((size_t)argc, sizeof *args);
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        args[i] = (char *)malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], n, NULL, NULL);
    }
    int result = test_main(argc, args);
    for (int i = 0; i < argc; ++i) free(args[i]);
    free(args);
    return result;
}
#else
int main(int argc, char **argv) { return test_main(argc, argv); }
#endif
