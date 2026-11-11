"""Compile the actual RomFS selection/path checks; redirects and writable mounts must stay outside the pack."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


HARNESS = r'''
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "@HEADER@"
#define HOS_FS_PATH 1024
#define FS_PATH_MAX 0x302
typedef struct { char name[32]; char root[HOS_FS_PATH]; int writable; } Mount;
static Mount mounts[3] = {{"rom", "base", 0}, {"save", "base", 1}, {"other", "elsewhere", 0}};
static int nmounts = 3;
static SRWLOCK mount_lock = SRWLOCK_INIT;
static const char *hos_romfs_dir = "base";
static RomfsPackFile romfs_image;
static size_t dummy(void *ctx, uint64_t off, void *out, size_t n) {
    (void)ctx; (void)off; (void)out; (void)n; return 0;
}
@ACTUAL@
static int bad, checks;
static void check(const char *guest, const char *resolved, int selected, const char *expected) {
    char rel[FS_PATH_MAX];
    memset(rel, 0x55, sizeof rel);
    int got = pack_path(guest, resolved, rel);
    ++checks;
    if (got != selected || (got && strcmp(rel, expected))) {
        printf("FAIL %s -> %s: selected %d, expected %d\n", guest, resolved, got, selected);
        bad = 1;
    }
}
int main(void) {
    check("rom:/a", "base\\a", 0, "");
    romfs_image.pack.read = dummy;
    check("rom:/a", "base\\a", 1, "a");
    check("rom:/dir/file", "base\\dir\\file", 1, "dir/file");
    check("rom:////a//b/", "base\\a\\\\b\\", 1, "a/b");
    check("rom:/", "base\\", 1, "");
    check("rom:/a", "mods\\a", 0, "");
    check("rom:/UI/layout", "generated\\wide-layout", 0, "");
    check("rom:/UI/prompt", "generated\\prompt", 0, "");
    check("rom:/a", "overlay/a", 0, "");
    check("save:/a", "base\\a", 0, "");
    check("other:/a", "elsewhere\\a", 0, "");
    check("rom-extra:/a", "base\\a", 0, "");
    check("rom/a", "base\\a", 0, "");
    check("rom:/../a", "base\\..\\a", 0, "");
    check("rom:/a\\b", "base\\a\\b", 0, "");
    check("rom:/a:b", "base\\a:b", 0, "");
#ifdef _WIN32
    check("rom:/NUL .txt", "base\\NUL .txt", 0, "");
#endif
    hos_romfs_dir = NULL;
    check("rom:/a", "base\\a", 0, "");
    if (!bad) printf("%d mount-selection cases passed\n", checks);
    return bad;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    text = (ROOT/'host/nn_fs.c').read_text(encoding='utf-8')
    actual = block(text, 'static int guest_path_ok(') + '\n' + block(text, 'static int pack_path(')
    header = str(ROOT/'host/romfs_pack_file.h').replace('\\', '/')
    with tempfile.TemporaryDirectory(prefix='romfs-mount-') as tmp:
        folder = Path(tmp)
        def run(source):
            path = folder/'fixture.c'
            path.write_text(HARNESS.replace('@HEADER@', header).replace('@ACTUAL@', source), encoding='utf-8')
            exe = folder/('mount.exe' if os.name == 'nt' else 'mount')
            cmd = [args.cxx, '-x', 'c', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)]
            if os.name != 'nt':
                cmd += ['-I'+str(ROOT/'host/posix'), '-pthread']
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                                    creationflags=0x08000000 if os.name == 'nt' else 0)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=15,
                                  creationflags=0x08000000 if os.name == 'nt' else 0)
        result = run(actual)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        print(result.stdout.strip())
        if args.mutations:
            mutations = [('redirect-precedence', 'if (strcmp(base, resolved)) return 0;', '(void)resolved;'),
                         ('writable-mount', '!mounts[i].writable && ', '')]
            for name, old, new in mutations:
                assert actual.count(old) == 1
                result = run(actual.replace(old, new))
                assert result.returncode == 1, (name, result.returncode, result.stdout)
                print('rejected mutation: ' + name)


if __name__ == '__main__':
    main()
